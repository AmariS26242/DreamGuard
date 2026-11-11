package com.dreamguard.app.debug

import android.Manifest
import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.Looper
import android.os.Process
import android.os.SystemClock
import androidx.annotation.RequiresApi
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import java.util.Locale
import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.roundToInt

/**
 * DG-050 minimal playback-capture probe (debug source set only).
 *
 * Deliberate limits:
 *  - API 29+ only. One foreground service per run, `START_NOT_STICKY`, no boot
 *    receiver, no automatic retry or resume.
 *  - Order on every run: visible Activity obtains RECORD_AUDIO and a fresh
 *    MediaProjection consent -> service enters the mediaProjection FGS ->
 *    `getMediaProjection` -> register `onStop` -> only then create AudioRecord.
 *  - No VirtualDisplay, no microphone. AudioRecord is built exclusively from
 *    `AudioPlaybackCaptureConfiguration` matched to the requested UID and
 *    USAGE_MEDIA / USAGE_GAME / USAGE_UNKNOWN.
 *  - Aggregate statistics only; no PCM buffer is retained, saved or logged.
 *  - Optional low-gain monitor playback is off by default. It never requests
 *    audio focus, never writes the system media volume, never changes the source
 *    player and never calls `setPreferredDevice`. Its own attributes forbid
 *    capture so it cannot be captured back into this probe.
 *  - A run is hard-capped at 30 seconds. User stop, system revocation, timeout,
 *    read failure and setup failure all end in one explicit cleanup path.
 */
@RequiresApi(Build.VERSION_CODES.Q)
class Dg050ProbeService : Service() {

    private data class Session(
        val runId: String,
        val sourceUid: Int,
        val monitor: Boolean,
        val startedAtMs: Long,
    )

    private data class CaptureResult(
        val failure: String?,
        val endReason: String,
        val frames: Long,
        val nonzero: Long,
        val peak: Int,
        val durationMs: Long,
        val monitor: String,
    )

    private val mainHandler = Handler(Looper.getMainLooper())

    private var captureThread: HandlerThread? = null

    @Volatile
    private var projection: MediaProjection? = null
    private var projectionCallback: MediaProjection.Callback? = null
    private var foregroundStarted = false
    private var starting = false
    private var session: Session? = null
    private var timeoutRunnable: Runnable? = null

    private val stopRequested = AtomicBoolean(false)

    @Volatile
    private var stopReason: String = "unknown"

    @Volatile
    private var audioRecord: AudioRecord? = null

    @Volatile
    private var monitorTrack: AudioTrack? = null

    @Volatile
    private var monitorState: String = "off"

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> beginSession(intent)
            ACTION_STOP -> {
                if (session == null && !starting) {
                    Dg050ProbeLog.info("event=stop_ignored reason=no_active_session")
                    stopSelf()
                } else {
                    requestStop("user_stop")
                }
            }
            else -> Dg050ProbeLog.warn("event=ignored_intent action=${intent?.action ?: "null"}")
        }
        return START_NOT_STICKY
    }

    private fun beginSession(intent: Intent) {
        if (starting || session != null) {
            Dg050ProbeLog.info(
                "event=start_rejected reason=already_running run=${session?.runId ?: "none"}",
            )
            return
        }
        starting = true
        stopRequested.set(false)
        stopReason = "unknown"
        monitorState = "off"

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            failStart("unsupported_api", "api=${Build.VERSION.SDK_INT} required=29")
            return
        }

        val sourceUid = intent.getIntExtra(EXTRA_SOURCE_UID, 0)
        if (sourceUid <= 0) {
            failStart("invalid_uid", "source_uid=$sourceUid")
            return
        }
        if (sourceUid == Process.myUid()) {
            failStart("self_uid", "source_uid=$sourceUid")
            return
        }
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            failStart("permission_missing", "permission=RECORD_AUDIO")
            return
        }

        val resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, Activity.RESULT_CANCELED)
        val consentData = projectionData(intent)
        if (resultCode != Activity.RESULT_OK || consentData == null) {
            failStart("missing_consent", "result_code=$resultCode")
            return
        }

        val monitor = intent.getBooleanExtra(EXTRA_MONITOR, false)
        val runId = newRunId()
        val pending = Session(
            runId = runId,
            sourceUid = sourceUid,
            monitor = monitor,
            startedAtMs = SystemClock.elapsedRealtime(),
        )

        // API 34+ enforcement: enter the mediaProjection FGS before asking for
        // the token. The consent intent was already approved by the user.
        if (!enterForeground(pending)) {
            failStart("foreground_failed", "type=mediaProjection")
            return
        }

        val manager = getSystemService(MediaProjectionManager::class.java)
        if (manager == null) {
            failStart("projection_manager_missing", "manager=null")
            return
        }
        val created = try {
            manager.getMediaProjection(resultCode, consentData)
        } catch (failure: Throwable) {
            failStart(
                "projection_failed",
                "type=${failure.javaClass.simpleName} message=${sanitize(failure.message)}",
            )
            return
        }
        if (created == null) {
            failStart("projection_null", "projection=null")
            return
        }
        projection = created

        // Register onStop before any AudioRecord exists; a revoked projection
        // must end the capture instead of leaving a silent reader running.
        val callback = object : MediaProjection.Callback() {
            override fun onStop() {
                if (stopRequested.get()) return
                requestStop("system_revoked")
            }
        }
        projectionCallback = callback
        try {
            created.registerCallback(callback, mainHandler)
        } catch (failure: Throwable) {
            failStart(
                "projection_callback_failed",
                "type=${failure.javaClass.simpleName} message=${sanitize(failure.message)}",
            )
            return
        }

        if (stopRequested.get()) {
            starting = false
            cleanupProjection()
            releaseForeground()
            Dg050ProbeLog.info("event=stop_requested reason=user_stop stage=before_capture run=$runId")
            Dg050ProbeBus.setRunning(false, "已在捕获前停止。", finished = true)
            stopSelf()
            return
        }

        session = pending
        starting = false
        Dg050ProbeBus.setRunning(
            running = true,
            line = "会话 $runId 启动中：UID $sourceUid，监视=${if (monitor) "开" else "关"}。",
        )
        Dg050ProbeLog.info(
            "event=session_starting run=$runId source_uid=$sourceUid monitor=$monitor " +
                "max_ms=$MAX_CAPTURE_MS usage_filter=media,game,unknown " +
                "virtual_display=none microphone=none",
        )
        scheduleTimeout(pending)

        val thread = HandlerThread("dg050-capture-$runId").also { it.start() }
        captureThread = thread
        Handler(thread.looper).post { runCapture(pending) }
    }

    private fun enterForeground(session: Session): Boolean = try {
        createNotificationChannel()
        ServiceCompat.startForeground(
            this,
            NOTIFICATION_ID,
            buildNotification(session),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION,
        )
        foregroundStarted = true
        Dg050ProbeLog.info("event=foreground_started run=${session.runId} type=mediaProjection")
        true
    } catch (failure: Throwable) {
        Dg050ProbeLog.error(
            "event=error stage=foreground type=${failure.javaClass.simpleName} " +
                "message=${sanitize(failure.message)}",
        )
        false
    }

    private fun failStart(reason: String, detail: String) {
        val suffix = if (detail.isBlank()) "" else " $detail"
        Dg050ProbeLog.error("event=error stage=start reason=$reason$suffix")
        starting = false
        stopRequested.set(false)
        cleanupProjection()
        releaseForeground()
        Dg050ProbeBus.setRunning(false, "启动失败：$reason", finished = true)
        stopSelf()
    }

    private fun scheduleTimeout(session: Session) {
        val runnable = Runnable {
            if (this.session === session) requestStop("timeout")
        }
        timeoutRunnable = runnable
        mainHandler.postDelayed(runnable, MAX_CAPTURE_MS)
    }

    /** One-shot; every caller converges here and only the first reason is recorded. */
    private fun requestStop(reason: String) {
        if (!stopRequested.compareAndSet(false, true)) return
        stopReason = reason
        val active = session
        if (active != null) {
            Dg050ProbeLog.info(
                "event=$reason run=${active.runId} source_uid=${active.sourceUid} " +
                    "elapsed_ms=${SystemClock.elapsedRealtime() - active.startedAtMs}",
            )
            Dg050ProbeBus.setRunning(true, "停止中：$reason")
        }
        // Unblock a blocking read/write. A system revocation can also make the
        // read fail on its own; both paths end in the same finally block.
        runCatching { audioRecord?.stop() }
        runCatching { monitorTrack?.pause() }
    }

    private fun runCapture(session: Session) {
        var localRecord: AudioRecord? = null
        var localTrack: AudioTrack? = null
        var failure: String? = null
        var captureStartedAtMs = 0L
        val stats = Dg050CaptureStats(CHANNEL_COUNT)

        try {
            val token = projection ?: throw IllegalStateException("projection_missing")
            val requestedFormat = AudioFormat.Builder()
                .setEncoding(ENCODING)
                .setSampleRate(SAMPLE_RATE)
                .setChannelMask(CHANNEL_MASK_IN)
                .build()

            val captureConfig = AudioPlaybackCaptureConfiguration.Builder(token)
                .addMatchingUid(session.sourceUid)
                .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                .addMatchingUsage(AudioAttributes.USAGE_GAME)
                .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                .build()

            val minBufferBytes = AudioRecord.getMinBufferSize(
                SAMPLE_RATE,
                CHANNEL_MASK_IN,
                ENCODING,
            )
            if (minBufferBytes <= 0) throw IllegalStateException("min_buffer=$minBufferBytes")
            val bufferBytes = max(
                minBufferBytes * 2,
                SAMPLE_RATE * CHANNEL_COUNT * 2 / 20,
            )

            val record = AudioRecord.Builder()
                .setAudioFormat(requestedFormat)
                .setAudioPlaybackCaptureConfig(captureConfig)
                .setBufferSizeInBytes(bufferBytes)
                .build()
            localRecord = record
            audioRecord = record

            if (record.state != AudioRecord.STATE_INITIALIZED) {
                throw IllegalStateException("state=${record.state}")
            }
            if (stopRequested.get()) return

            record.startRecording()
            if (record.recordingState != AudioRecord.RECORDSTATE_RECORDING) {
                throw IllegalStateException("recording_state=${record.recordingState}")
            }
            if (record.channelCount != CHANNEL_COUNT) {
                throw IllegalStateException("unexpected_channels=${record.channelCount}")
            }
            captureStartedAtMs = SystemClock.elapsedRealtime()
            Dg050ProbeLog.info(
                "event=capture_started run=${session.runId} source_uid=${session.sourceUid} " +
                    "requested_rate=$SAMPLE_RATE requested_channels=$CHANNEL_COUNT " +
                    "requested_encoding=pcm16 actual_rate=${record.sampleRate} " +
                    "actual_channels=${record.channelCount} " +
                    "actual_encoding=${encodingName(record.audioFormat)} " +
                    "buffer_bytes=$bufferBytes monitor=${if (session.monitor) "requested" else "off"} " +
                    "usage_filter=media,game,unknown virtual_display=none microphone=none",
            )

            if (session.monitor) {
                localTrack = startMonitor(session, record)
            }

            val buffer = ShortArray(bufferBytes / 2)
            val loopStartedAtMs = SystemClock.elapsedRealtime()
            var lastReportAtMs = loopStartedAtMs
            var latencySumMs = 0.0
            var latencyMaxMs = 0.0
            var latencyBlocks = 0L

            while (!stopRequested.get()) {
                val nowMs = SystemClock.elapsedRealtime()
                if (nowMs - session.startedAtMs >= MAX_CAPTURE_MS) {
                    requestStop("timeout")
                    break
                }
                // Report from the loop condition, not only after a read: a
                // silent/empty capture must still produce 1 Hz observation logs.
                if (nowMs - lastReportAtMs >= REPORT_INTERVAL_MS) {
                    if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) !=
                        PackageManager.PERMISSION_GRANTED
                    ) {
                        requestStop("permission_revoked")
                        break
                    }
                    logStats(
                        session = session,
                        record = record,
                        stats = stats,
                        elapsedMs = nowMs - loopStartedAtMs,
                        latencySumMs = latencySumMs,
                        latencyMaxMs = latencyMaxMs,
                        latencyBlocks = latencyBlocks,
                    )
                    lastReportAtMs = nowMs
                }

                val read = try {
                    // Non-blocking plus a short idle sleep keeps the timeout and
                    // 1 Hz stats alive even when the platform supplies no frames.
                    record.read(buffer, 0, buffer.size, AudioRecord.READ_NON_BLOCKING)
                } catch (failureDuringRead: Throwable) {
                    if (!stopRequested.get()) {
                        failure = "read_exception type=${failureDuringRead.javaClass.simpleName} " +
                            "message=${sanitize(failureDuringRead.message)}"
                    }
                    break
                }
                if (read < 0) {
                    if (!stopRequested.get()) failure = "read_error code=$read"
                    break
                }
                if (read == 0) {
                    SystemClock.sleep(IDLE_READ_SLEEP_MS)
                    continue
                }

                val readDoneNs = SystemClock.elapsedRealtimeNanos()
                stats.add(buffer, read)

                val track = monitorTrack
                if (track != null) {
                    val written = try {
                        track.write(buffer, 0, read, AudioTrack.WRITE_BLOCKING)
                    } catch (writeFailure: Throwable) {
                        -1
                    }
                    val writeDoneNs = SystemClock.elapsedRealtimeNanos()
                    if (written < 0) {
                        // A paused track during user/system stop is not a
                        // monitor failure; only report a real write error while
                        // capture is still supposed to be running.
                        if (!stopRequested.get()) {
                            monitorState = "failed"
                            monitorTrack = null
                            Dg050ProbeLog.warn(
                                "event=monitor_stopped run=${session.runId} reason=write_error " +
                                    "code=$written capture_continues=1 playback_started=0",
                            )
                            runCatching {
                                track.pause()
                                track.flush()
                            }
                        }
                    } else if (written > 0) {
                        latencyBlocks++
                        val latencyMs = (writeDoneNs - readDoneNs) / 1_000_000.0
                        latencySumMs += latencyMs
                        if (latencyMs > latencyMaxMs) latencyMaxMs = latencyMs
                    }
                }
            }
        } catch (failureDuringCapture: Throwable) {
            if (!stopRequested.get()) {
                failure = "capture_exception type=${failureDuringCapture.javaClass.simpleName} " +
                    "message=${sanitize(failureDuringCapture.message)}"
            }
        } finally {
            audioRecord = null
            monitorTrack = null
            runCatching { localRecord?.stop() }
            runCatching { localRecord?.release() }
            runCatching { localTrack?.stop() }
            runCatching { localTrack?.release() }

            val totals = stats.totals()
            val result = CaptureResult(
                failure = failure,
                endReason = if (failure != null) "error" else stopReason,
                frames = totals.frames,
                nonzero = totals.nonzero,
                peak = totals.peak,
                durationMs = if (captureStartedAtMs == 0L) {
                    0L
                } else {
                    SystemClock.elapsedRealtime() - captureStartedAtMs
                },
                monitor = monitorState,
            )
            mainHandler.post { finishSession(session, result) }
        }
    }

    private fun startMonitor(session: Session, record: AudioRecord): AudioTrack? {
        var track: AudioTrack? = null
        return try {
            // ALLOW_CAPTURE_BY_NONE: the monitor must not be captured by this or
            // any other playback-capture app (no loopback). No AudioFocusRequest
            // and no AudioManager volume write are used.
            val attributes = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .setAllowedCapturePolicy(AudioAttributes.ALLOW_CAPTURE_BY_NONE)
                .build()
            val format = AudioFormat.Builder()
                .setEncoding(ENCODING)
                .setSampleRate(record.sampleRate)
                .setChannelMask(CHANNEL_MASK_OUT)
                .build()
            val minBufferBytes = AudioTrack.getMinBufferSize(
                record.sampleRate,
                CHANNEL_MASK_OUT,
                ENCODING,
            )
            if (minBufferBytes <= 0) throw IllegalStateException("min_buffer=$minBufferBytes")
            val trackBufferBytes = max(
                minBufferBytes * 2,
                record.sampleRate * CHANNEL_COUNT * 2 / 20,
            )

            val created = AudioTrack.Builder()
                .setAudioAttributes(attributes)
                .setAudioFormat(format)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setBufferSizeInBytes(trackBufferBytes)
                .build()
            track = created

            val volumeStatus = created.setVolume(MONITOR_GAIN)
            if (volumeStatus != AudioTrack.SUCCESS) {
                throw IllegalStateException("set_volume_status=$volumeStatus")
            }
            created.play()
            if (created.playState != AudioTrack.PLAYSTATE_PLAYING) {
                throw IllegalStateException("play_state=${created.playState}")
            }

            monitorTrack = created
            monitorState = "on"
            Dg050ProbeLog.info(
                "event=monitor_started run=${session.runId} gain=$MONITOR_GAIN focus=requested_none " +
                    "system_volume_write=none preferred_device=none capture_policy=none " +
                    "route=${routeOf(created)} lat_scope=app_read_to_write_enqueue",
            )
            created
        } catch (failure: Throwable) {
            monitorState = "failed"
            runCatching { track?.release() }
            Dg050ProbeLog.warn(
                "event=monitor_start_failed run=${session.runId} " +
                    "type=${failure.javaClass.simpleName} message=${sanitize(failure.message)} " +
                    "capture_continues=1 playback_started=0",
            )
            null
        }
    }

    private fun logStats(
        session: Session,
        record: AudioRecord,
        stats: Dg050CaptureStats,
        elapsedMs: Long,
        latencySumMs: Double,
        latencyMaxMs: Double,
        latencyBlocks: Long,
    ) {
        val interval = stats.takeInterval()
        val state = if (interval.nonzeroTotal == 0L) "SILENT" else "CAPTURING"
        val rmsDbfs = if (interval.rmsInterval <= 0.0) {
            "none"
        } else {
            String.format(
                Locale.US,
                "%.1f",
                20.0 * log10(interval.rmsInterval / 32768.0),
            )
        }
        val latency = if (latencyBlocks > 0) {
            String.format(
                Locale.US,
                "lat_avg_ms=%.2f lat_max_ms=%.2f lat_blocks=%d lat_scope=app_read_to_write_enqueue",
                latencySumMs / latencyBlocks,
                latencyMaxMs,
                latencyBlocks,
            )
        } else {
            "lat_avg_ms=none lat_max_ms=none lat_blocks=0 lat_scope=app_read_to_write_enqueue"
        }
        val route = when {
            monitorState == "on" -> monitorTrack?.let { routeOf(it) } ?: "unavailable"
            monitorState == "failed" -> "unavailable"
            else -> "none"
        }
        val silenceNote = if (state == "SILENT") {
            " note=continuous_zero_is_observation_not_source_rejection"
        } else {
            ""
        }
        Dg050ProbeLog.stats(
            "event=stats run=${session.runId} source_uid=${session.sourceUid} " +
                "actual_rate=${record.sampleRate} actual_channels=${record.channelCount} " +
                "actual_encoding=${encodingName(record.audioFormat)} " +
                "frames=${interval.framesTotal} nonzero=${interval.nonzeroTotal} " +
                "peak=${interval.peakTotal} nonzero_1s=${interval.nonzeroInterval} " +
                "peak_1s=${interval.peakInterval} rms_1s=${interval.rmsInterval.roundToInt()} " +
                "rms_dbfs_1s=$rmsDbfs elapsed_ms=$elapsedMs state=$state " +
                "capture_error=0 monitor=$monitorState route=$route $latency$silenceNote",
        )
    }

    private fun finishSession(session: Session, result: CaptureResult) {
        if (this.session !== session) return
        this.session = null
        starting = false
        timeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        timeoutRunnable = null
        captureThread?.quitSafely()
        captureThread = null
        cleanupProjection()
        releaseForeground()

        if (result.failure != null) {
            Dg050ProbeLog.error(
                "event=error run=${session.runId} source_uid=${session.sourceUid} " +
                    "stage=capture reason=${result.failure}",
            )
        }
        val signal = if (result.nonzero > 0L) "present" else "none"
        Dg050ProbeLog.info(
            "event=capture_finished run=${session.runId} source_uid=${session.sourceUid} " +
                "reason=${result.endReason} duration_ms=${result.durationMs} " +
                "frames=${result.frames} nonzero=${result.nonzero} peak=${result.peak} " +
                "monitor=${result.monitor} signal=$signal",
        )
        if (result.nonzero == 0L && result.failure == null) {
            Dg050ProbeLog.info(
                "event=observation run=${session.runId} source_uid=${session.sourceUid} " +
                    "note=continuous_zero_is_observation_not_source_rejection",
            )
        }
        val summary = "会话结束：${result.endReason} · frames=${result.frames} · " +
            "nonzero=${result.nonzero} · monitor=${result.monitor}"
        Dg050ProbeBus.setRunning(false, summary, finished = true)
        stopSelf()
    }

    private fun cleanupProjection() {
        val activeProjection = projection
        val callback = projectionCallback
        projection = null
        projectionCallback = null
        if (activeProjection != null) {
            if (callback != null) runCatching { activeProjection.unregisterCallback(callback) }
            runCatching { activeProjection.stop() }
        }
    }

    private fun releaseForeground() {
        if (!foregroundStarted) return
        foregroundStarted = false
        runCatching { stopForeground(Service.STOP_FOREGROUND_REMOVE) }
    }

    override fun onDestroy() {
        val wasActive = session != null
        stopRequested.set(true)
        timeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        timeoutRunnable = null
        runCatching { audioRecord?.stop() }
        runCatching { monitorTrack?.pause() }
        captureThread?.quitSafely()
        captureThread = null
        cleanupProjection()
        releaseForeground()
        session = null
        if (wasActive) {
            Dg050ProbeLog.warn("event=service_destroyed note=capture_did_not_finish_normally")
            Dg050ProbeBus.setRunning(false, "服务被销毁，捕获未正常收尾。", finished = true)
        }
        super.onDestroy()
    }

    private fun createNotificationChannel() {
        val manager = getSystemService(NotificationManager::class.java) ?: return
        if (manager.getNotificationChannel(CHANNEL_ID) != null) return
        val channel = NotificationChannel(
            CHANNEL_ID,
            "DG-050 捕获探针",
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = "仅 Debug：最长 30 秒的播放捕获实验"
            setShowBadge(false)
        }
        manager.createNotificationChannel(channel)
    }

    private fun buildNotification(session: Session): Notification {
        val openApp = PendingIntent.getActivity(
            this,
            REQUEST_OPEN,
            Intent(this, Dg050ProbeActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
            },
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val stop = PendingIntent.getService(
            this,
            REQUEST_STOP,
            Intent(this, Dg050ProbeService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle("DG-050 捕获探针 · UID ${session.sourceUid}")
            .setContentText("捕获中，最长 ${MAX_CAPTURE_MS / 1000} 秒；仅 Debug。")
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setContentIntent(openApp)
            .addAction(android.R.drawable.ic_menu_close_clear_cancel, "停止", stop)
            .build()
    }

    @Suppress("DEPRECATION")
    private fun projectionData(intent: Intent): Intent? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(EXTRA_PROJECTION_DATA, Intent::class.java)
        } else {
            intent.getParcelableExtra(EXTRA_PROJECTION_DATA)
        }

    private fun routeOf(track: AudioTrack): String {
        val device = runCatching { track.routedDevice }.getOrNull() ?: return "unavailable"
        val rawName = device.productName?.toString().orEmpty()
        val name = rawName.replace(Regex("\\s+"), "_").ifBlank { "unnamed" }
        return "${deviceTypeName(device.type)}(id=${device.id},name=$name)"
    }

    private fun deviceTypeName(type: Int): String = when (type) {
        android.media.AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "builtin_speaker"
        android.media.AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> "builtin_earpiece"
        android.media.AudioDeviceInfo.TYPE_WIRED_HEADSET -> "wired_headset"
        android.media.AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "wired_headphones"
        android.media.AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "bluetooth_a2dp"
        android.media.AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> "bluetooth_sco"
        android.media.AudioDeviceInfo.TYPE_USB_DEVICE -> "usb_device"
        android.media.AudioDeviceInfo.TYPE_USB_HEADSET -> "usb_headset"
        android.media.AudioDeviceInfo.TYPE_HDMI -> "hdmi"
        android.media.AudioDeviceInfo.TYPE_HDMI_ARC -> "hdmi_arc"
        android.media.AudioDeviceInfo.TYPE_DOCK -> "dock"
        android.media.AudioDeviceInfo.TYPE_LINE_ANALOG -> "line_analog"
        android.media.AudioDeviceInfo.TYPE_LINE_DIGITAL -> "line_digital"
        android.media.AudioDeviceInfo.TYPE_HEARING_AID -> "hearing_aid"
        android.media.AudioDeviceInfo.TYPE_BLE_HEADSET -> "ble_headset"
        android.media.AudioDeviceInfo.TYPE_BLE_SPEAKER -> "ble_speaker"
        else -> "type_$type"
    }

    private fun encodingName(encoding: Int): String = when (encoding) {
        AudioFormat.ENCODING_PCM_8BIT -> "pcm8"
        AudioFormat.ENCODING_PCM_16BIT -> "pcm16"
        AudioFormat.ENCODING_PCM_FLOAT -> "pcm_float"
        else -> "encoding_$encoding"
    }

    private fun sanitize(message: String?): String =
        (message ?: "none").replace(Regex("[\\r\\n\\t ]+"), "_").take(180)

    private fun newRunId(): String = UUID.randomUUID().toString().take(8)

    companion object {
        const val ACTION_START = "com.dreamguard.app.debug.action.DG050_START"
        const val ACTION_STOP = "com.dreamguard.app.debug.action.DG050_STOP"

        const val EXTRA_PROJECTION_DATA = "projection_data"
        const val EXTRA_RESULT_CODE = "result_code"
        const val EXTRA_SOURCE_UID = "source_uid"
        const val EXTRA_MONITOR = "monitor"

        private const val CHANNEL_ID = "dg050_probe"
        private const val NOTIFICATION_ID = 5050
        private const val REQUEST_OPEN = 501
        private const val REQUEST_STOP = 502

        private const val MAX_CAPTURE_MS = 30_000L
        private const val REPORT_INTERVAL_MS = 1_000L
        private const val IDLE_READ_SLEEP_MS = 10L
        private const val SAMPLE_RATE = 48_000
        private const val CHANNEL_COUNT = 2
        private const val MONITOR_GAIN = 0.15f

        private val CHANNEL_MASK_IN = AudioFormat.CHANNEL_IN_STEREO
        private val CHANNEL_MASK_OUT = AudioFormat.CHANNEL_OUT_STEREO
        private val ENCODING = AudioFormat.ENCODING_PCM_16BIT
    }
}
