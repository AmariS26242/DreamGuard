package com.dreamguard.app.audio

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Process

/**
 * Production [AudioEngine]: renders float PCM on a dedicated thread through
 * [AudioTrack] and pulls samples from the C++20 DSP core via JNI.
 *
 * Library name, JNI symbols, and the CMake target all use `dreamguard_dsp`.
 */
class NativeAudioEngine : AudioEngine {

    private val lock = Any()

    @Volatile
    private var currentState: EngineState = EngineState.IDLE

    @Volatile
    private var running = false

    @Volatile
    private var released = false

    private var handle: Long = 0L
    private var track: AudioTrack? = null
    private var renderThread: Thread? = null

    private var desiredGain = AudioSettings.DEFAULT_GAIN
    private var desiredCutoffHz = AudioSettings.DEFAULT_TONE_CUTOFF_HZ
    private var desiredNoiseMix = AudioSettings.DEFAULT_NOISE_MIX
    private var desiredDroneHz = AudioSettings.DEFAULT_DRONE_HZ

    override val isAvailable: Boolean
        get() = isNativeLibraryLoaded && !released

    override val state: EngineState
        get() = currentState

    override fun start(): Boolean {
        if (!isNativeLibraryLoaded) {
            currentState = EngineState.ERROR
            return false
        }
        synchronized(lock) {
            if (released) {
                currentState = EngineState.ERROR
                return false
            }
            if (currentState == EngineState.RUNNING) return true

            if (handle == 0L) {
                handle = runCatching { nativeCreate(SAMPLE_RATE, CHANNELS) }.getOrDefault(0L)
                if (handle == 0L) {
                    currentState = EngineState.ERROR
                    return false
                }
            }

            val minBufferBytes = AudioTrack.getMinBufferSize(
                SAMPLE_RATE,
                AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_FLOAT,
            )
            if (minBufferBytes <= 0) {
                currentState = EngineState.ERROR
                return false
            }

            val newTrack = try {
                AudioTrack.Builder()
                    .setAudioAttributes(
                        AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                            .build(),
                    )
                    .setAudioFormat(
                        AudioFormat.Builder()
                            .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                            .setSampleRate(SAMPLE_RATE)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                            .build(),
                    )
                    .setBufferSizeInBytes(minBufferBytes * 2)
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .build()
            } catch (_: Throwable) {
                currentState = EngineState.ERROR
                return false
            }

            try {
                applyNativeParametersLocked()
                newTrack.play()
            } catch (_: Throwable) {
                runCatching { newTrack.release() }
                currentState = EngineState.ERROR
                return false
            }

            track = newTrack
            running = true
            val thread = Thread({ renderLoop(handle, newTrack) }, "dreamguard-audio").apply {
                priority = Thread.NORM_PRIORITY + 1
                start()
            }
            renderThread = thread
            currentState = EngineState.RUNNING
            return true
        }
    }

    override fun stop() {
        val threadToJoin: Thread?
        synchronized(lock) {
            if (released) return
            running = false
            threadToJoin = renderThread
            renderThread = null
            track?.let { audioTrack ->
                runCatching {
                    audioTrack.pause()
                    audioTrack.flush()
                    audioTrack.stop()
                }
                runCatching { audioTrack.release() }
            }
            track = null
            if (handle != 0L) {
                runCatching { nativeReset(handle) }
            }
            currentState = EngineState.STOPPED
        }
        if (threadToJoin != null && threadToJoin !== Thread.currentThread()) {
            runCatching { threadToJoin.join(500) }
        }
    }

    override fun release() {
        if (released) return
        stop()
        synchronized(lock) {
            if (released) return
            released = true
            running = false
            if (handle != 0L) {
                runCatching { nativeDestroy(handle) }
                handle = 0L
            }
            currentState = EngineState.RELEASED
        }
    }

    override fun setGain(gain: Float) {
        if (!gain.isFinite()) return
        val safe = gain.coerceIn(AudioSettings.MIN_GAIN, AudioSettings.MAX_GAIN)
        desiredGain = safe
        withHandle { nativeSetGain(it, safe) }
    }

    override fun setToneCutoff(cutoffHz: Float) {
        if (!cutoffHz.isFinite()) return
        val safe = cutoffHz.coerceIn(
            AudioSettings.MIN_TONE_CUTOFF_HZ,
            AudioSettings.MAX_TONE_CUTOFF_HZ,
        )
        desiredCutoffHz = safe
        withHandle { nativeSetToneCutoff(it, safe) }
    }

    override fun setNoiseMix(mix: Float) {
        if (!mix.isFinite()) return
        val safe = mix.coerceIn(AudioSettings.MIN_NOISE_MIX, AudioSettings.MAX_NOISE_MIX)
        desiredNoiseMix = safe
        withHandle { nativeSetNoiseMix(it, safe) }
    }

    override fun setDroneHz(hz: Float) {
        if (!hz.isFinite()) return
        val safe = hz.coerceIn(AudioSettings.MIN_DRONE_HZ, AudioSettings.MAX_DRONE_HZ)
        desiredDroneHz = safe
        withHandle { nativeSetDroneHz(it, safe) }
    }

    /** DSP core version reported by the native library, for the UI footer. */
    fun versionLabel(): String = if (isNativeLibraryLoaded) {
        runCatching { nativeVersion() }.getOrDefault("unknown")
    } else {
        "unavailable"
    }

    private fun renderLoop(handle: Long, audioTrack: AudioTrack) {
        Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO)
        val buffer = FloatArray(FRAMES_PER_BLOCK)
        try {
            while (running) {
                val frames = nativeRender(handle, buffer, FRAMES_PER_BLOCK)
                if (frames <= 0) break
                var offset = 0
                while (offset < frames && running) {
                    val written = audioTrack.write(
                        buffer,
                        offset,
                        frames - offset,
                        AudioTrack.WRITE_BLOCKING,
                    )
                    if (written < 0) {
                        running = false
                        break
                    }
                    offset += written
                }
            }
        } catch (_: Throwable) {
            running = false
            currentState = EngineState.ERROR
        }
    }

    private fun applyNativeParametersLocked() {
        if (handle == 0L) return
        runCatching {
            nativeSetGain(handle, desiredGain)
            nativeSetToneCutoff(handle, desiredCutoffHz)
            nativeSetNoiseMix(handle, desiredNoiseMix)
            nativeSetDroneHz(handle, desiredDroneHz)
        }
    }

    private inline fun withHandle(block: (Long) -> Unit) {
        val current = handle
        if (current == 0L || !isNativeLibraryLoaded) return
        runCatching { block(current) }
    }

    private external fun nativeCreate(sampleRate: Int, channels: Int): Long
    private external fun nativeDestroy(handle: Long)
    private external fun nativeReset(handle: Long)
    private external fun nativeRender(handle: Long, buffer: FloatArray, frames: Int): Int
    private external fun nativeSetGain(handle: Long, gain: Float)
    private external fun nativeSetToneCutoff(handle: Long, cutoffHz: Float)
    private external fun nativeSetNoiseMix(handle: Long, mix: Float)
    private external fun nativeSetDroneHz(handle: Long, hz: Float)
    private external fun nativeVersion(): String

    companion object {
        const val LIBRARY_NAME = "dreamguard_dsp"
        const val SAMPLE_RATE = 48_000
        const val CHANNELS = 1
        private const val FRAMES_PER_BLOCK = 1024

        val isNativeLibraryLoaded: Boolean by lazy {
            runCatching { System.loadLibrary(LIBRARY_NAME) }.isSuccess
        }
    }
}
