package com.dreamguard.app.audio

/**
 * Platform-independent playback state machine. It validates settings, forwards
 * them to the [AudioEngine], and exposes a snapshot that Compose can observe.
 * Keeping this class free of Android imports is what makes it JVM-testable.
 */
class PlaybackController(
    private val engine: AudioEngine,
    initialSettings: AudioSettings = AudioSettings(),
) {
    var settings: AudioSettings = initialSettings.sanitized()
        private set

    // Engine callbacks arrive on the render thread while the UI reads these on
    // main, so both flags must be safe to publish across threads.
    @Volatile
    var isRunning: Boolean = false
        private set

    @Volatile
    var lastError: String? = null
        private set

    /**
     * True when the last [start] was rejected because the previous render thread was
     * still shutting down. That is not a failure: the engine is healthy, it simply
     * needs a moment, so the service retries with a bound and the UI can offer an
     * explicit "play" again instead of silently dropping the user's choice.
     */
    @Volatile
    var retryableStart: Boolean = false
        private set

    /** Classification of [lastError]; [EngineFailureKind.NONE] for a deferred start. */
    @Volatile
    var failureKind: EngineFailureKind = EngineFailureKind.NONE
        private set

    /**
     * Incremented once for every engine failure report, so observers (the service)
     * can react exactly once per failure instead of on every unrelated update.
     */
    @Volatile
    var engineFailureCount: Int = 0
        private set

    /** Called after every state or settings change; the UI replaces this. */
    var listener: (() -> Unit)? = null

    val isAvailable: Boolean
        get() = engine.isAvailable

    val engineState: EngineState
        get() = engine.state

    init {
        // The render thread can fail (or be torn down) long after start() returned, so
        // the controller mirrors engine state instead of trusting its own flag.
        engine.onStateChanged = { syncWithEngine() }
    }

    private fun syncWithEngine() {
        when (engine.state) {
            EngineState.RUNNING -> {
                isRunning = true
                lastError = null
                retryableStart = false
                failureKind = EngineFailureKind.NONE
            }
            EngineState.ERROR -> {
                isRunning = false
                retryableStart = false
                lastError = engine.lastFailure ?: "Audio engine stopped unexpectedly."
                failureKind = engine.failureKind
                engineFailureCount++
            }
            EngineState.STOPPED, EngineState.IDLE, EngineState.RELEASED -> isRunning = false
        }
        notifyListener()
    }

    fun start(): Boolean {
        retryableStart = false
        failureKind = EngineFailureKind.NONE
        if (engine.state == EngineState.RELEASED) {
            return fail("Audio engine has already been released.")
        }
        if (!engine.isAvailable) {
            return fail("Native DSP library is unavailable for this device ABI.")
        }

        applySettings()
        val started = runCatching { engine.start() }.getOrElse { error ->
            fail("Audio engine failed to start: ${error.message ?: "unknown error"}")
            return false
        }
        // The render thread may fail between engine.start() returning and this line, so
        // derive the flag from the engine state instead of trusting `started` alone.
        isRunning = started && engine.state == EngineState.RUNNING
        when {
            isRunning -> {
                lastError = null
                failureKind = EngineFailureKind.NONE
            }
            // A rejection while the old render thread is still exiting is a transient
            // "try again shortly", not a failure the user caused or must lose data for.
            !started && engine.state == EngineState.STOPPED -> {
                lastError = null
                retryableStart = true
                failureKind = EngineFailureKind.NONE
            }
            else -> {
                lastError = engine.lastFailure ?: "Audio output could not be opened."
                failureKind = engine.failureKind
            }
        }
        notifyListener()
        return isRunning
    }

    /**
     * Drops a deferred start that the caller has given up on (retry bound reached, the
     * user switched layers, or playback was cancelled). Keeps [lastError] untouched so
     * the service can replace it with a visible explanation.
     */
    fun abandonDeferredStart() {
        if (!retryableStart) return
        retryableStart = false
        notifyListener()
    }

    fun stop() {
        retryableStart = false
        if (!isRunning && engine.state != EngineState.RUNNING) {
            notifyListener()
            return
        }
        runCatching { engine.stop() }
        isRunning = false
        failureKind = EngineFailureKind.NONE
        notifyListener()
    }

    fun toggle(): Boolean = if (isRunning) {
        stop()
        false
    } else {
        start()
    }

    fun setGain(gain: Float) {
        updateSettings(settings.copy(gain = gain))
    }

    fun setToneCutoff(cutoffHz: Float) {
        updateSettings(settings.copy(toneCutoffHz = cutoffHz))
    }

    fun setNoiseMix(mix: Float) {
        updateSettings(settings.copy(noiseMix = mix))
    }

    fun setDroneHz(hz: Float) {
        updateSettings(settings.copy(droneHz = hz))
    }

    fun release() {
        engine.onStateChanged = null
        runCatching { engine.release() }
        isRunning = false
        retryableStart = false
        failureKind = EngineFailureKind.NONE
        lastError = null
        notifyListener()
    }

    fun snapshot(): PlaybackSnapshot = PlaybackSnapshot(
        settings = settings,
        isRunning = isRunning,
        engineState = engine.state,
        isAvailable = engine.isAvailable,
        lastError = lastError,
        retryableStart = retryableStart,
        failureKind = failureKind,
    )

    private fun updateSettings(candidate: AudioSettings) {
        val sanitized = candidate.sanitized()
        settings = sanitized
        applySettings()
        notifyListener()
    }

    private fun applySettings() {
        runCatching {
            engine.setGain(settings.gain)
            engine.setToneCutoff(settings.toneCutoffHz)
            engine.setNoiseMix(settings.noiseMix)
            engine.setDroneHz(settings.droneHz)
        }
    }

    private fun fail(message: String): Boolean {
        isRunning = false
        lastError = message
        notifyListener()
        return false
    }

    private fun notifyListener() {
        runCatching { listener?.invoke() }
    }
}

data class PlaybackSnapshot(
    val settings: AudioSettings,
    val isRunning: Boolean,
    val engineState: EngineState,
    val isAvailable: Boolean,
    val lastError: String?,
    val retryableStart: Boolean = false,
    val failureKind: EngineFailureKind = EngineFailureKind.NONE,
)
