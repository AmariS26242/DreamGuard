package com.dreamguard.app.audio

/**
 * JVM test double. Records forwarded parameters and lets each test decide
 * whether start() succeeds.
 */
class FakeAudioEngine(
    override val isAvailable: Boolean = true,
    var startResult: Boolean = true,
) : AudioEngine {

    override var state: EngineState = EngineState.IDLE
        private set

    val calls = mutableListOf<String>()

    var lastGain: Float? = null
    var lastCutoffHz: Float? = null
    var lastNoiseMix: Float? = null
    var lastDroneHz: Float? = null

    override fun start(): Boolean {
        calls += "start"
        if (!isAvailable || !startResult) return false
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
