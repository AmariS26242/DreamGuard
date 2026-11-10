package com.dreamguard.app.audio

/**
 * JVM test double. Records forwarded parameters and lets each test decide
 * whether start() succeeds or is deferred by a still-shutting-down session.
 */
class FakeAudioEngine(
    override val isAvailable: Boolean = true,
    var startResult: Boolean = true,
    /** Mirrors [NativeAudioEngine] rejecting a start while the old thread is alive. */
    var startDeferred: Boolean = false,
) : AudioEngine {

    override var state: EngineState = EngineState.IDLE
        private set

    override var onStateChanged: (() -> Unit)? = null

    override var lastFailure: String? = null
        private set

    override var failureKind: EngineFailureKind = EngineFailureKind.NONE
        private set

    val calls = mutableListOf<String>()

    var lastGain: Float? = null
    var lastCutoffHz: Float? = null
    var lastNoiseMix: Float? = null
    var lastDroneHz: Float? = null

    /** Simulates the render thread reporting a failure after start() returned true. */
    fun failAsync(
        message: String = "render thread failed",
        kind: EngineFailureKind = EngineFailureKind.TRANSIENT,
    ) {
        calls += "fail"
        lastFailure = message
        failureKind = kind
        state = EngineState.ERROR
        onStateChanged?.invoke()
    }

    /** Simulates the render thread finishing on its own (e.g. output error). */
    fun stopAsync() {
        calls += "stopAsync"
        state = EngineState.STOPPED
        onStateChanged?.invoke()
    }

    override fun start(): Boolean {
        calls += "start"
        if (startDeferred) {
            // The real engine stays STOPPED and reports no failure kind here.
            lastFailure = "The previous audio session is still shutting down."
            failureKind = EngineFailureKind.NONE
            state = EngineState.STOPPED
            return false
        }
        if (!isAvailable || !startResult) return false
        lastFailure = null
        failureKind = EngineFailureKind.NONE
        state = EngineState.RUNNING
        return true
    }

    override fun stop() {
        calls += "stop"
        state = EngineState.STOPPED
    }

    override fun setGain(gain: Float) {
        calls += "gain=$gain"
        lastGain = gain
    }

    override fun setToneCutoff(cutoffHz: Float) {
        calls += "cutoff=$cutoffHz"
        lastCutoffHz = cutoffHz
    }

    override fun setNoiseMix(mix: Float) {
        calls += "noise=$mix"
        lastNoiseMix = mix
    }

    override fun setDroneHz(hz: Float) {
        calls += "drone=$hz"
        lastDroneHz = hz
    }

    override fun release() {
        calls += "release"
        state = EngineState.RELEASED
    }
}
