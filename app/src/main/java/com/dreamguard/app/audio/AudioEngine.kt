package com.dreamguard.app.audio

enum class EngineState {
    IDLE,
    RUNNING,
    STOPPED,
    ERROR,
    RELEASED,
}

/**
 * Why the engine failed last time. The distinction is user-visible policy, not an
 * attempt to predict the future: an output that could not be opened or a write that
 * failed is usually caused by a transient route change (Bluetooth hand-off, focus
 * change) and must not erase the user's saved layer choice, while a missing library
 * or a rejected native creation is not going to fix itself.
 */
enum class EngineFailureKind {
    NONE,
    TRANSIENT,
    PERMANENT,
}

/**
 * Test seam for audio playback. [NativeAudioEngine] is the production
 * implementation; JVM unit tests use a fake instead of loading the NDK library.
 */
interface AudioEngine {
    val isAvailable: Boolean
    val state: EngineState

    /**
     * Invoked when [state] changes without the caller asking for it, for example
     * when the render thread fails after [start] returned `true`. This may run on
     * the audio thread, so implementations of the callback must be thread-safe and
     * are expected to hop to the main thread before touching UI state.
     */
    var onStateChanged: (() -> Unit)?

    /** Human-readable description of the last asynchronous failure, if any. */
    val lastFailure: String?
        get() = null

    /** Classification of [lastFailure]; [EngineFailureKind.NONE] when nothing failed. */
    val failureKind: EngineFailureKind
        get() = EngineFailureKind.NONE

    fun start(): Boolean
    fun stop()
    fun setGain(gain: Float)
    fun setToneCutoff(cutoffHz: Float)
    fun setNoiseMix(mix: Float)
    fun setDroneHz(hz: Float)
    fun release()
}
