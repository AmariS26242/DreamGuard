package com.dreamguard.app.audio

enum class EngineState {
    IDLE,
    RUNNING,
    STOPPED,
    ERROR,
    RELEASED,
}

/**
 * Test seam for audio playback. [NativeAudioEngine] is the production
 * implementation; JVM unit tests use a fake instead of loading the NDK library.
 */
interface AudioEngine {
    val isAvailable: Boolean
    val state: EngineState

    fun start(): Boolean
    fun stop()
    fun setGain(gain: Float)
    fun setToneCutoff(cutoffHz: Float)
    fun setNoiseMix(mix: Float)
    fun setDroneHz(hz: Float)
    fun release()
}
