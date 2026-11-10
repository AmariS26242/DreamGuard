package com.dreamguard.app.audio

import android.app.PendingIntent
import android.content.Intent
import android.net.Uri
import android.os.Binder
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.session.MediaSession
import androidx.media3.session.MediaSessionService
import com.dreamguard.app.MainActivity
import com.dreamguard.app.R
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture

/**
 * One player owns audio focus and the media session for both layers.
 *
 * Layer selection ([PlaybackLayers]) is persisted so a service that the system
 * recreates — media button after the task was removed, focus loss, or process
 * death — never reports "playing" while producing silence.
 */
class PlaybackService : MediaSessionService() {
    inner class LocalBinder : Binder() { val service: PlaybackService get() = this@PlaybackService }

    private val binder = LocalBinder()
    private val mainHandler by lazy { Handler(Looper.getMainLooper()) }
    private val prefs by lazy { getSharedPreferences(PREFS_NAME, MODE_PRIVATE) }

    private lateinit var player: ExoPlayer
    private lateinit var session: MediaSession
    private lateinit var engine: NativeAudioEngine
    lateinit var controller: PlaybackController
        private set

    private var layers = PlaybackLayers()

    /** Last engine failure already reconciled with the player, see [syncPlayerWithRenderFailure]. */
    private var handledFailureCount = 0

    /**
     * Set immediately before the service pauses the player for an internal failure, and
     * consumed by the resulting `playWhenReady = false` callback. This is what keeps
     * such a pause from being mistaken for a user request and ending the service.
     */
    private var pausedForInternalFailure = false

    /** Automatic follow-up starts for a DSP start deferred by a slow shutdown. */
    private var dspStartAttempts = 0
    private var retryScheduled = false
    private val retryDspStart = Runnable {
        retryScheduled = false
        startDsp()
    }

    var listener: (() -> Unit)? = null
    var error: String? = null
        private set

    /** Whether the Media3 loop is the user's selected sound layer (not whether it is playing). */
    val ambientEnabled: Boolean get() = layers.ambient

    val version: String get() = engine.versionLabel()

    override fun onCreate() {
        super.onCreate()
        layers = PlaybackLayers(
            dsp = prefs.getBoolean(KEY_DSP_SELECTED, false),
            ambient = prefs.getBoolean(KEY_AMBIENT_SELECTED, false),
        )

        engine = NativeAudioEngine()
        controller = PlaybackController(engine)
        // Engine failures surface on the render thread; Compose state must change on main.
        controller.listener = { dispatchToListener() }

        player = ExoPlayer.Builder(this).build().apply {
            // Ask ExoPlayer to pause rather than duck: its focus decision also
            // controls the native layer, which ExoPlayer cannot attenuate.
            setAudioAttributes(AudioAttributes.Builder().setUsage(C.USAGE_MEDIA)
                .setContentType(C.AUDIO_CONTENT_TYPE_SPEECH).build(), true)
            setHandleAudioBecomingNoisy(true)
            repeatMode = Player.REPEAT_MODE_ONE
            volume = layers.ambientVolume
            setMediaItem(defaultMediaItem())
            prepare()
            addListener(object : Player.Listener {
                override fun onIsPlayingChanged(isPlaying: Boolean) {
                    if (isPlaying) {
                        pausedForInternalFailure = false
                        when {
                            // Media button after a restart with no layer selected: never
                            // leave a silent foreground playback session behind.
                            layers.isEmpty -> pause()
                            layers.dsp -> startDsp()
                        }
                    } else if (controller.isRunning) {
                        controller.stop()
                    }
                    dispatchToListener()
                }

                override fun onPlayWhenReadyChanged(playWhenReady: Boolean, reason: Int) {
                    if (playWhenReady) {
                        pausedForInternalFailure = false
                    } else {
                        cancelDspStartRetry()
                        // The user (or focus loss) cancelled the session: no deferred
                        // start may keep waiting or resurface after this pause.
                        controller.abandonDeferredStart()
                        // Consume the internal-cleanup marker on the pause it caused, so
                        // a later genuine user pause still ends the session normally.
                        val internalCleanup = pausedForInternalFailure
                        pausedForInternalFailure = false
                        if (PlaybackFailurePolicy.shouldStopServiceOnPause(
                                isUserRequest =
                                    reason == Player.PLAY_WHEN_READY_CHANGE_REASON_USER_REQUEST,
                                isInternalCleanup = internalCleanup,
                            )
                        ) {
                            stopSelf()
                        }
                    }
                    dispatchToListener()
                }

                override fun onPlayerError(playbackError: PlaybackException) {
                    error = "音频播放失败：${playbackError.message ?: "未知错误"}"
                    cancelDspStartRetry()
                    // Not a user pause: keep the session and the visible error alive.
                    pauseForInternalFailure()
                    dispatchToListener()
                }
            })
        }
        session = MediaSession.Builder(this, player)
            .setSessionActivity(buildSessionActivity())
            .setCallback(sessionCallback)
            .build()
        addSession(session)
    }

    override fun onBind(intent: Intent?): IBinder? =
        if (intent?.action == ACTION_BIND_UI) {
            if (Binder.getCallingUid() == applicationInfo.uid) binder else null
        } else super.onBind(intent)

    override fun onGetSession(controllerInfo: MediaSession.ControllerInfo): MediaSession = session

    fun toggleDsp() {
        // "Selected" is not "running": a DSP layer whose start failed or is still
        // waiting for the old thread must be retried by the play button, not silently
        // deselected. Only an actually active (or actively waiting) layer can be paused.
        if (layers.dsp && (controller.isRunning || controller.retryableStart)) {
            layers = layers.withoutDsp()
            persistLayers()
            cancelDspStartRetry()
            controller.stop()
            if (!layers.ambient) player.pause()
        } else {
            layers = layers.withDsp()
            persistLayers()
            startPlayback()
            // The ambient loop may already be playing, in which case no isPlaying
            // callback fires and the DSP has to be started here. A start that was
            // deferred by a slow shutdown is retried from here as well, so the user's
            // play press is always an explicit retry.
            if (player.isPlaying && !controller.isRunning) startDsp()
        }
        dispatchToListener()
    }

    fun toggleAmbient() {
        layers = if (layers.ambient) layers.withoutAmbient() else layers.withAmbient()
        persistLayers()
        player.volume = layers.ambientVolume
        if (layers.ambient) {
            startPlayback()
            if (player.isPlaying && !controller.isRunning) startDsp()
        } else if (!layers.dsp) {
            // No layer left: this pause is the user's intent, so the session may end.
            cancelDspStartRetry()
            player.pause()
        } else if (controller.engineState == EngineState.ERROR && player.playWhenReady) {
            // With the loop switched off, a failed DSP layer is not audible either:
            // never leave the player running as a silent foreground session.
            pauseForInternalFailure()
        }
        dispatchToListener()
    }

    private fun startPlayback() {
        error = null
        pausedForInternalFailure = false
        cancelDspStartRetry()
        runCatching { startService(Intent(this, PlaybackService::class.java)) }
            .onFailure { error = "后台播放启动失败：${it.message ?: "系统拒绝前台服务"}" }
        // ExoPlayer stays in an error state until prepare() is called again; without
        // this, the visible "play" retry after a player error would do nothing.
        if (player.playerError != null) {
            runCatching { player.prepare() }
        }
        player.play()
    }

    /**
     * Starts the DSP layer, retrying a bounded number of times while the previous
     * render thread is still shutting down. Exhausting the retries surfaces a visible,
     * retryable message instead of silently clearing the user's layer selection.
     */
    private fun startDsp() {
        if (retryScheduled) return
        if (!PlaybackRecoveryPolicy.shouldAttemptStart(
                layers = layers,
                wantsPlayback = player.playWhenReady,
                alreadyRunning = controller.isRunning,
            )
        ) {
            return
        }

        if (controller.start()) {
            error = null
            dspStartAttempts = 0
            return
        }
        if (controller.isRunning) {
            dspStartAttempts = 0
            return
        }

        if (PlaybackRecoveryPolicy.shouldRetry(
                retryableStart = controller.retryableStart,
                layers = layers,
                wantsPlayback = player.playWhenReady,
                retriesSoFar = dspStartAttempts,
            )
        ) {
            dspStartAttempts++
            retryScheduled = true
            mainHandler.postDelayed(retryDspStart, PlaybackRecoveryPolicy.RETRY_DELAY_MS)
            return
        }

        val deferred = controller.retryableStart
        dspStartAttempts = 0
        controller.abandonDeferredStart()
        if (deferred) {
            error = "上一段音频仍在关闭，请稍后重试。"
        }
        if (player.playWhenReady && PlaybackFailurePolicy.shouldPausePlayer(layers)) {
            pauseForInternalFailure()
        }
        dispatchToListener()
    }

    private fun cancelDspStartRetry() {
        retryScheduled = false
        dspStartAttempts = 0
        mainHandler.removeCallbacks(retryDspStart)
    }

    /**
     * Pauses the player because of an internal failure. The marker is consumed by the
     * resulting `playWhenReady = false` callback, which therefore does not call
     * [stopSelf] and keeps the error visible.
     */
    private fun pauseForInternalFailure() {
        // If the player is already paused there is no callback to consume the marker;
        // leaving it set would swallow the next genuine user pause and keep the
        // foreground service (and its notification) alive.
        if (!player.playWhenReady) return
        pausedForInternalFailure = true
        player.pause()
    }

    /**
     * Render-thread failures are reported from the audio thread; the player may only
     * be touched on main. Without this, a DSP-only session keeps a foreground media
     * notification alive while playing silence.
     *
     * Reacting to [PlaybackController.engineFailureCount] rather than to the error text
     * keeps this idempotent: unrelated UI updates, focus changes and an in-flight
     * retry never pause the player, and each failure is handled exactly once.
     */
    private fun syncPlayerWithRenderFailure() {
        val failures = controller.engineFailureCount
        if (failures == handledFailureCount) return
        handledFailureCount = failures
        if (controller.engineState != EngineState.ERROR) return
        if (!PlaybackFailurePolicy.shouldPausePlayer(layers)) return
        if (!player.playWhenReady) return
        pauseForInternalFailure()
    }

    private fun dispatchToListener() {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            syncPlayerWithRenderFailure()
            listener?.invoke()
        } else {
            mainHandler.post {
                syncPlayerWithRenderFailure()
                listener?.invoke()
            }
        }
    }

    private fun persistLayers() {
        prefs.edit()
            .putBoolean(KEY_DSP_SELECTED, layers.dsp)
            .putBoolean(KEY_AMBIENT_SELECTED, layers.ambient)
            .apply()
    }

    private fun buildSessionActivity(): PendingIntent {
        val intent = Intent(this, MainActivity::class.java).apply {
            action = Intent.ACTION_MAIN
            addCategory(Intent.CATEGORY_LAUNCHER)
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        return PendingIntent.getActivity(
            this,
            0,
            intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
    }

    private fun defaultMediaItem(): MediaItem {
        val uri = Uri.parse("android.resource://$packageName/${R.raw.dream_loop}")
        return MediaItem.Builder()
            .setUri(uri)
            .setMediaId(MEDIA_ID)
            .setMediaMetadata(MediaMetadata.Builder().setTitle("DreamGuard 声景").build())
            .build()
    }

    /** System media resumption (e.g. after process death) rebuilds the same single item. */
    private val sessionCallback = object : MediaSession.Callback {
        override fun onPlaybackResumption(
            mediaSession: MediaSession,
            controller: MediaSession.ControllerInfo,
        ): ListenableFuture<MediaSession.MediaItemsWithStartPosition> =
            Futures.immediateFuture(
                MediaSession.MediaItemsWithStartPosition(listOf(defaultMediaItem()), 0, 0L),
            )
    }

    override fun onDestroy() {
        cancelDspStartRetry()
        listener = null
        controller.listener = null
        controller.release()
        session.release()
        player.release()
        super.onDestroy()
    }

    companion object {
        const val ACTION_BIND_UI = "com.dreamguard.app.BIND_UI"
        private const val PREFS_NAME = "dreamguard.playback"
        private const val KEY_DSP_SELECTED = "dsp_selected"
        private const val KEY_AMBIENT_SELECTED = "ambient_selected"
        private const val MEDIA_ID = "dreamguard"
    }
}
