package com.dreamguard.app.audio

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.AudioAttributes
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.net.Uri
import android.os.Binder
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import androidx.core.app.NotificationCompat
import com.dreamguard.app.MainActivity
import com.dreamguard.app.R
import java.util.UUID
import java.util.concurrent.atomic.AtomicInteger

/**
 * DG-025 foreground service: holds the countdown while the app is backgrounded or
 * the screen is locked, and owns the only timer thread.
 *
 * Deliberate limits:
 *  - `specialUse` (API 34+) rather than `mediaPlayback`: this service never starts
 *    DreamGuard audio, never creates an AudioTrack and never requests audio focus.
 *    It only calls the public `STREAM_MUSIC` read/write used by DG-024.
 *  - `START_NOT_STICKY`, no boot receiver: a killed process is not resurrected in
 *    the background. The persisted active marker becomes an explicit
 *    "interrupted, nothing was written back" prompt on the next launch.
 *  - No exact alarm and no wake lock: ticks run on a HandlerThread and may be
 *    delayed by Doze/OEM background policy. A late tick writes only the level due
 *    at that moment (never an out-of-order level).
 *  - Non-exported; `onBind` additionally checks the calling UID.
 *
 * One serial owner: start/tick/cancel/stop decisions and every engine call run on
 * [timerHandler]. `currentSession` is assigned there, so a late main-thread
 * finalize from an old run cannot match a newly accepted session. The shared
 * [SleepTimerWriteBarrier] and a per-session notification token keep old tasks
 * and old notifications from cancelling or writing into the next run.
 */
class SleepTimerService : Service() {

    inner class LocalBinder : Binder() {
        val service: SleepTimerService get() = this@SleepTimerService
    }

    private val binder = LocalBinder()
    private val mainHandler = Handler(Looper.getMainLooper())
    private lateinit var timerThread: HandlerThread
    private lateinit var timerHandler: Handler
    private lateinit var store: SleepTimerStore
    private lateinit var recovery: SleepTimerRecovery
    private lateinit var gate: SleepTimerGate
    private lateinit var writeBarrier: SleepTimerWriteBarrier
    private lateinit var volume: ControllerSleepTimerVolume
    private lateinit var engine: SleepTimerEngine

    private var audioManager: AudioManager? = null
    private var volumeObserver: SystemVolumeObserver? = null
    private var deviceCallbackSeen = false

    @Volatile
    private var lastStatus: SleepTimerStatus = SleepTimerStatus()

    @Volatile
    private var foregroundStarted = false

    @Volatile
    private var finalized = true

    @Volatile
    private var destroyed = false

    @Volatile
    private var currentSession = 0L

    /** True while a start is being decided on the timer thread. */
    @Volatile
    private var startInProgress = false

    /** Only touched on [timerHandler]. */
    private var tickingSession = 0L

    /**
     * Starts queued but not yet picked up by the timer thread. A stale cancel may
     * not stopSelf while one is queued, or it could kill a visible start before
     * its session exists.
     */
    private val pendingStarts = AtomicInteger(0)

    /** Called on main when a status/terminal change should be shown in the UI. */
    var listener: (() -> Unit)? = null

    private val tickRunnable = object : Runnable {
        override fun run() = runTick()
    }

    private val deviceCallback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>) {
            onAudioDevicesChanged(addedDevices)
        }

        override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>) {
            onAudioDevicesChanged(removedDevices)
        }
    }

    override fun onCreate() {
        super.onCreate()
        store = SleepTimerStore(applicationContext)
        recovery = SleepTimerRecovery(store)
        gate = SleepTimerRuntime.gate()
        writeBarrier = SleepTimerRuntime.writeBarrier()
        audioManager = getSystemService(Context.AUDIO_SERVICE) as? AudioManager
        volume = ControllerSleepTimerVolume(SystemVolumeController(AudioManagerVolumeBackend(applicationContext)))
        timerThread = HandlerThread("dreamguard-sleep-timer").also { it.start() }
        timerHandler = Handler(timerThread.looper)
        engine = SleepTimerEngine(
            volume = volume,
            clock = SleepTimerClock { SystemClock.elapsedRealtime() },
            gate = gate,
            writeBarrier = writeBarrier,
            outputSignature = { currentOutputSignature() },
        )
        createNotificationChannel()
        // Faster than one tick of latency, but only a hint: the authoritative
        // external-change check is the read inside SleepTimerEngine.tick().
        volumeObserver = SystemVolumeObserver(this) { requestTickNow() }.also { it.start() }
        runCatching { audioManager?.registerAudioDeviceCallback(deviceCallback, mainHandler) }
        SleepTimerRuntime.attach(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> {
                val config = intent?.let { configFromIntent(it) }
                if (config == null) {
                    // A malformed duplicate start must never tear down a timer
                    // that is already running.
                    if (!SleepTimerRuntime.isActive) {
                        publishStartRejection(
                            SleepTimerStartProblem.INVALID_DURATION,
                            SleepTimerPolicy.startProblemMessage(SleepTimerStartProblem.INVALID_DURATION),
                        )
                    }
                } else {
                    // Single start path: the visible Activity always starts the
                    // service with this intent; the binder is only used to read
                    // status and to cancel a specific session.
                    startTimer(config)
                }
            }
            ACTION_CANCEL -> cancelTimer(intent?.getLongExtra(EXTRA_SESSION, -1L) ?: -1L)
            else -> Unit
        }
        // Never restart a countdown after a process kill; the persisted marker
        // becomes an "interrupted" prompt instead.
        return Service.START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? {
        // Service.onBind is abstract; only our explicit UI-bind action is valid.
        // Notification actions use getService, not bind, so everything else is
        // safely rejected with null.
        if (intent?.action != ACTION_BIND_UI) return null
        return if (Binder.getCallingUid() == applicationInfo.uid) binder else null
    }

    fun currentStatus(): SleepTimerStatus = lastStatus

    /**
     * Arms a timer from [onStartCommand]. All run state and every engine call are
     * on the single timer thread; only the foreground/notification/UI side
     * effects move to main.
     */
    private fun startTimer(config: SleepTimerConfig) {
        pendingStarts.incrementAndGet()
        timerHandler.post { startTimerOnSerialOwner(config) }
    }

    /** Runs only on [timerHandler]. */
    private fun startTimerOnSerialOwner(config: SleepTimerConfig) {
        startInProgress = true
        // This start is now being decided; only later queued starts remain
        // "pending" for stale-cancel/old-finalize guards. onDestroy resets the
        // counter, so do not decrement below it there.
        if (!destroyed) pendingStarts.decrementAndGet()
        try {
            decideStart(config)
        } finally {
            startInProgress = false
        }
    }

    private fun decideStart(config: SleepTimerConfig) {
        if (destroyed) return
        val result = engine.start(config)
        val status = engine.status
        if (result.accepted) {
            val session = newSessionToken()
            currentSession = session
            finalized = false
            writeBarrier.setEnabled(true)

            // Durable active marker BEFORE the first tick/volume write. This
            // runs on the timer thread, so it never blocks the UI thread.
            val marked = recovery.markActive(status.config ?: config)
            if (!marked) {
                // This start's own failure must be allowed to finalize even
                // though the start decision is still unwinding.
                startInProgress = false
                val failed = engine.stop(
                    SleepTimerOutcome.SERVICE_FAILED,
                    "无法持久化睡眠定时的活跃标记，定时未启动。",
                )
                writeBarrier.setEnabled(false)
                mainHandler.post { finalizeTerminal(failed, session) }
                return
            }

            mainHandler.post {
                if (destroyed || session != currentSession || finalized) return@post
                lastStatus = status.copy(session = session)
                SleepTimerRuntime.setActive(true)
                val foreground = enterForeground(session)
                listener?.invoke()
                if (foreground) {
                    timerHandler.post { startTicking(session) }
                }
            }
        } else if (result.problem == SleepTimerStartProblem.ALREADY_RUNNING) {
            // A second start does not disturb the running timer.
            mainHandler.post { listener?.invoke() }
        } else {
            currentSession = 0L
            finalized = true
            writeBarrier.setEnabled(false)
            recovery.markTerminal(
                SleepTimerOutcome.START_REJECTED,
                status.message ?: SleepTimerPolicy.startProblemMessage(
                    result.problem ?: SleepTimerStartProblem.INVALID_DURATION,
                ),
                System.currentTimeMillis(),
            )
            startInProgress = false
            mainHandler.post {
                if (destroyed) return@post
                // A newer start may have been accepted after this rejection was
                // decided; its state must not be overwritten by this old result.
                if (currentSession != 0L || pendingStarts.get() > 0 || startInProgress) return@post
                lastStatus = status
                SleepTimerRuntime.setActive(false)
                listener?.invoke()
                stopSelf()
            }
        }
    }

    /**
     * Cancels one specific run. [sessionToken] comes from the notification
     * PendingIntent or the card's status; a token from an older run is ignored,
     * so a stale notification can never cancel the next timer.
     */
    fun cancelTimer(sessionToken: Long) {
        if (sessionToken != currentSession) {
            // Stale notification/card action. Never touch a newer run's
            // gate/barrier. Only release a service instance that has no run and no
            // start in flight (a stale PendingIntent may have created it).
            if (currentSession == 0L &&
                pendingStarts.get() == 0 &&
                !startInProgress &&
                !foregroundStarted &&
                !SleepTimerRuntime.isActive
            ) {
                stopSelf()
            }
            return
        }
        if (startInProgress && !SleepTimerRuntime.isActive && !foregroundStarted) {
            // A new start is being decided and no run is active yet: this token
            // belongs to the previous run, so it must not touch the new one.
            return
        }
        // Synchronous, before the queued cancel task runs: a tick already waiting
        // on the serial queue must not write after this point.
        gate.invalidate(SleepTimerOutcome.CANCELLED)
        writeBarrier.setEnabled(false)
        timerHandler.post {
            if (destroyed) return@post
            val session = currentSession
            if (session != sessionToken) return@post
            val status = engine.status
            if (!status.isRunning) {
                if (status.outcome != null) {
                    mainHandler.post { finalizeTerminal(status, session) }
                } else {
                    mainHandler.post {
                        if (!foregroundStarted && pendingStarts.get() == 0 && !startInProgress) {
                            stopSelf()
                        }
                    }
                }
                return@post
            }
            val stopped = engine.stop(
                SleepTimerOutcome.CANCELLED,
                "已取消睡眠定时；音量保持在当前值，不恢复旧音量。",
            )
            mainHandler.post { finalizeTerminal(stopped, session) }
        }
    }

    /**
     * The in-app system-volume slider was used. [SleepTimerRuntime] already
     * invalidated the gate, disabled timer writes and holds the shared barrier
     * while this is called; this only makes the terminal state visible.
     */
    fun onInAppVolumeChange() {
        // Reentrant when called from the slider's barrier; idempotent otherwise.
        writeBarrier.setEnabled(false)
        timerHandler.post {
            if (destroyed) return@post
            val session = currentSession
            val status = engine.status
            if (!status.isRunning) {
                if (status.outcome != null) {
                    mainHandler.post { finalizeTerminal(status, session) }
                }
                return@post
            }
            val stopped = engine.stop(
                SleepTimerOutcome.MANUAL_VOLUME_CHANGE,
                "应用内手动调节音量，定时已结束并保留当前音量。",
            )
            mainHandler.post { finalizeTerminal(stopped, session) }
        }
    }

    override fun onDestroy() {
        destroyed = true
        startInProgress = false
        writeBarrier.setEnabled(false)
        timerHandler.removeCallbacksAndMessages(null)
        runCatching { audioManager?.unregisterAudioDeviceCallback(deviceCallback) }
        runCatching { volumeObserver?.stop() }
        if (!finalized && SleepTimerRuntime.isActive) {
            // The service disappeared outside a normal terminal transition (for
            // example an OEM stop). Persist the interruption; never write/restore.
            SleepTimerRuntime.setActive(false)
            runCatching {
                recovery.markTerminal(
                    SleepTimerOutcome.INTERRUPTED,
                    "睡眠定时服务被系统停止，定时中断；没有恢复旧音量，也没有继续写入。",
                    System.currentTimeMillis(),
                )
            }
        }
        finalized = true
        currentSession = 0L
        pendingStarts.set(0)
        SleepTimerRuntime.detach(this)
        listener = null
        timerThread.quitSafely()
        super.onDestroy()
    }

    // ---- timer loop ------------------------------------------------------

    private fun startTicking(session: Long) {
        if (destroyed) return
        tickingSession = session
        timerHandler.removeCallbacks(tickRunnable)
        timerHandler.post(tickRunnable)
    }

    private fun runTick() {
        if (destroyed) return
        val session = tickingSession
        if (session != currentSession) return
        if (!engine.status.isRunning) return

        val status = engine.tick()
        mainHandler.post {
            if (destroyed || session != currentSession) return@post
            lastStatus = status.copy(session = session)
            if (status.isRunning) {
                updateNotification(status, session)
                listener?.invoke()
            } else {
                finalizeTerminal(status, session)
            }
        }
        if (status.isRunning && session == currentSession) {
            timerHandler.postDelayed(tickRunnable, TICK_INTERVAL_MS)
        }
    }

    private fun requestTickNow() {
        if (!SleepTimerRuntime.isActive) return
        timerHandler.removeCallbacks(tickRunnable)
        timerHandler.post(tickRunnable)
    }

    private fun finalizeTerminal(
        status: SleepTimerStatus,
        session: Long,
        force: Boolean = false,
    ) {
        if (destroyed) return
        // A newer start is already queued or being decided and will own the next
        // state; do not let this old finalize stop it. Foreground-start failures
        // force cleanup because no later start can supersede a still-"running"
        // engine.
        if (!force && (pendingStarts.get() > 0 || startInProgress)) return
        if (session != currentSession) return
        if (finalized) return
        finalized = true
        writeBarrier.setEnabled(false)
        foregroundStarted = false
        lastStatus = status.copy(session = session)
        SleepTimerRuntime.setActive(false)
        // Terminal persistence is not on a hot path; apply() avoids a main-thread
        // disk wait.
        recovery.markTerminal(
            status.outcome ?: SleepTimerOutcome.WRITE_FAILED,
            status.message ?: "睡眠定时已结束。",
            System.currentTimeMillis(),
        )
        timerHandler.removeCallbacks(tickRunnable)
        runCatching { stopForeground(Service.STOP_FOREGROUND_REMOVE) }
        listener?.invoke()
        stopSelf()
    }

    private fun publishStartRejection(problem: SleepTimerStartProblem, message: String) {
        // Re-check on the serial owner: a malformed/old start intent must not
        // tear down a run that became active after the main-thread fast check.
        timerHandler.post {
            if (destroyed ||
                startInProgress ||
                pendingStarts.get() > 0 ||
                engine.status.isRunning ||
                SleepTimerRuntime.isActive
            ) {
                return@post
            }
            val status = SleepTimerStatus(
                phase = SleepTimerPhase.IDLE,
                message = message,
                startProblem = problem,
            )
            currentSession = 0L
            finalized = true
            writeBarrier.setEnabled(false)
            // Non-durable: not on a hot path and never a main-thread disk wait.
            recovery.markTerminal(
                SleepTimerOutcome.START_REJECTED,
                message,
                System.currentTimeMillis(),
            )
            mainHandler.post {
                if (destroyed) return@post
                // A newer start may have been accepted after this rejection was
                // decided; its state must not be overwritten by this old result.
                if (currentSession != 0L || pendingStarts.get() > 0 || startInProgress) return@post
                lastStatus = status
                SleepTimerRuntime.setActive(false)
                listener?.invoke()
                stopSelf()
            }
        }
    }

    // ---- foreground notification ----------------------------------------

    private fun enterForeground(session: Long): Boolean = try {
        val notification = buildNotification(lastStatus, session)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE,
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
        foregroundStarted = true
        true
    } catch (failure: Throwable) {
        // Missing permission, OEM policy or a manifest mismatch: show a real
        // terminal failure instead of leaving a "running" state with no service.
        foregroundStarted = false
        writeBarrier.setEnabled(false)
        val message = "定时前台服务启动失败：${failure.message ?: "系统拒绝"}"
        timerHandler.post {
            val status = engine.stop(SleepTimerOutcome.SERVICE_FAILED, message)
            mainHandler.post { finalizeTerminal(status, session, force = true) }
        }
        false
    }

    private fun createNotificationChannel() {
        val manager = getSystemService(NotificationManager::class.java) ?: return
        if (manager.getNotificationChannel(CHANNEL_ID) != null) return
        val channel = NotificationChannel(
            CHANNEL_ID,
            "睡眠定时",
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = "睡眠定时剩余时间与取消入口"
            setShowBadge(false)
        }
        manager.createNotificationChannel(channel)
    }

    private fun updateNotification(status: SleepTimerStatus, session: Long) {
        if (!foregroundStarted || destroyed) return
        runCatching {
            getSystemService(NotificationManager::class.java)
                ?.notify(NOTIFICATION_ID, buildNotification(status, session))
        }
    }

    private fun buildNotification(status: SleepTimerStatus, session: Long): Notification {
        val openApp = PendingIntent.getActivity(
            this,
            REQUEST_OPEN,
            Intent(this, MainActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
            },
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        // PendingIntent identity includes the Intent data but NOT its extras.
        // Put the full positive token in the data URI: the constant request code
        // is then safe, and two runs can never share identity even across a
        // service/process rebuild. EXTRA_SESSION remains the value the service
        // validates against currentSession.
        val cancel = PendingIntent.getService(
            this,
            REQUEST_CANCEL,
            Intent(this, SleepTimerService::class.java)
                .setAction(ACTION_CANCEL)
                .setData(cancelIdentity(session))
                .putExtra(EXTRA_SESSION, session),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val title = when (status.phase) {
            SleepTimerPhase.HOLDING -> "睡眠定时 · 保持中"
            SleepTimerPhase.FADING -> "睡眠定时 · 渐弱中"
            else -> "睡眠定时"
        }
        val text = "剩余 ${formatRemaining(status.remainingMs)} · 当前 ${status.currentPercent}% · " +
            "目标 ${status.targetPercent}%"
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_sleep_timer)
            .setContentTitle(title)
            .setContentText(text)
            .setContentIntent(openApp)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
            .addAction(android.R.drawable.ic_menu_close_clear_cancel, "取消定时", cancel)
            .build()
    }

    private fun cancelIdentity(session: Long): Uri =
        Uri.parse("dreamguard://sleep-timer/cancel/$session")

    /**
     * Positive, non-zero token. A UUID-derived value is unique per run across
     * service instances and process rebuilds; it deliberately does not reuse an
     * in-instance counter that restarts at 1.
     */
    private fun newSessionToken(): Long {
        var token = 0L
        while (token == 0L) {
            token = UUID.randomUUID().mostSignificantBits and Long.MAX_VALUE
        }
        return token
    }

    // ---- output route detection -----------------------------------------

    private fun onAudioDevicesChanged(devices: Array<out AudioDeviceInfo>) {
        // The callback registration delivers the current snapshot first; it is
        // not a change. A source-only change (microphone) must not end a sleep
        // timer, so after the snapshot only sink changes count.
        if (!deviceCallbackSeen) {
            deviceCallbackSeen = true
            return
        }
        if (devices.none { it.isSink }) return
        if (!SleepTimerRuntime.isActive) return
        // Conservative: any added/removed output ends the countdown with zero
        // further writes, so the old route's target can never land on a new one.
        gate.invalidate(SleepTimerOutcome.OUTPUT_CHANGED)
        requestTickNow()
    }

    private fun currentOutputSignature(): String? {
        val manager = audioManager ?: return null
        return runCatching {
            val devices: List<AudioDeviceInfo> =
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    runCatching {
                        manager.getAudioDevicesForAttributes(
                            AudioAttributes.Builder()
                                .setUsage(AudioAttributes.USAGE_MEDIA)
                                .build(),
                        )
                    }.getOrNull()
                        ?: manager.getDevices(AudioManager.GET_DEVICES_OUTPUTS).toList()
                } else {
                    // Older systems: connected output devices are the best public
                    // approximation. Sources are filtered out below.
                    manager.getDevices(AudioManager.GET_DEVICES_OUTPUTS).toList()
                }
            devices.filter { it.isSink }
                .sortedWith(compareBy<AudioDeviceInfo> { it.type }.thenBy { it.id })
                .joinToString("|") { "${it.type}:${it.id}:${it.productName}" }
                .ifEmpty { null }
        }.getOrNull()
    }

    // ---- helpers ---------------------------------------------------------

    private fun configFromIntent(intent: Intent): SleepTimerConfig? {
        val total = intent.getLongExtra(EXTRA_TOTAL_MS, -1L)
        val fade = intent.getLongExtra(EXTRA_FADE_MS, -1L)
        val target = intent.getIntExtra(EXTRA_TARGET_INDEX, Int.MIN_VALUE)
        if (total <= 0L || fade < 0L || target == Int.MIN_VALUE) return null
        return SleepTimerConfig(total, fade, target)
    }

    companion object {
        const val ACTION_START = "com.dreamguard.app.action.SLEEP_TIMER_START"
        const val ACTION_CANCEL = "com.dreamguard.app.action.SLEEP_TIMER_CANCEL"
        const val ACTION_BIND_UI = "com.dreamguard.app.action.SLEEP_TIMER_BIND_UI"

        const val EXTRA_TOTAL_MS = "total_ms"
        const val EXTRA_FADE_MS = "fade_ms"
        const val EXTRA_TARGET_INDEX = "target_index"
        const val EXTRA_SESSION = "session"

        private const val CHANNEL_ID = "dreamguard_sleep_timer"
        private const val NOTIFICATION_ID = 25
        private const val REQUEST_OPEN = 1
        private const val REQUEST_CANCEL = 2
        private const val TICK_INTERVAL_MS = 1_000L
    }
}
