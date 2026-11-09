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

    var isRunning: Boolean = false
        private set

    var lastError: String? = null
        private set

    /** Called after every state or settings change; the UI replaces this. */
    var listener: (() -> Unit)? = null

    val isAvailable: Boolean
        get() = engine.isAvailable

    val engineState: EngineState
        get() = engine.state

    fun start(): Boolean {
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
        isRunning = started
        lastError = if (started) null else "Audio output could not be opened."
        notifyListener()
        return started
    }

    fun stop() {
        if (!isRunning && engine.state != EngineState.RUNNING) {
            notifyListener()
            return
        }
        runCatching { engine.stop() }
        isRunning = false
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
        runCatching { engine.release() }
        isRunning = false
        lastError = null
        notifyListener()
    }

    fun snapshot(): PlaybackSnapshot = PlaybackSnapshot(
        settings = settings,
        isRunning = isRunning,
        engineState = engine.state,
        isAvailable = engine.isAvailable,
        lastError = lastError,
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
)
