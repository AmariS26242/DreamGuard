package com.dreamguard.app.audio

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Process

/**
 * Native DSP entry points used by [NativeAudioEngine]. Kept behind an interface so
 * the engine's threading, error and teardown logic can be exercised on a plain JVM
 * without loading the NDK library.
 */
internal interface DspCommands {
    val isLoaded: Boolean

    fun create(sampleRate: Int, channels: Int): Long
    fun destroy(handle: Long)
    fun reset(handle: Long)
    fun render(handle: Long, buffer: FloatArray, frames: Int): Int
    fun setGain(handle: Long, gain: Float)
    fun setToneCutoff(handle: Long, cutoffHz: Float)
    fun setNoiseMix(handle: Long, mix: Float)
    fun setDroneHz(handle: Long, hz: Float)
    fun version(): String
}

/**
 * Float PCM output. Mirrors the subset of [AudioTrack] the render thread uses so a
 * test can substitute an output that blocks, fails, or counts calls.
 */
internal interface RenderOutput {
    fun play()

    /** @return frames written, or a negative [AudioTrack] error code. */
    fun write(buffer: FloatArray, offset: Int, size: Int): Int

    fun pause()
    fun flush()
    fun stop()
    fun release()
}

internal interface RenderOutputFactory {
    /** Runs on the render thread before the first block; sets audio priority in production. */
    fun prepareRenderThread()

    /** @return an open output, or null when the platform refuses the format or buffer size. */
    fun open(sampleRate: Int, channels: Int): RenderOutput?
}

internal class AndroidRenderOutputFactory : RenderOutputFactory {

    override fun prepareRenderThread() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO)
    }

    override fun open(sampleRate: Int, channels: Int): RenderOutput? {
        val channelMask = if (channels == 1) {
            AudioFormat.CHANNEL_OUT_MONO
        } else {
            AudioFormat.CHANNEL_OUT_STEREO
        }
        val minBufferBytes = AudioTrack.getMinBufferSize(
            sampleRate,
            channelMask,
            AudioFormat.ENCODING_PCM_FLOAT,
        )
        if (minBufferBytes <= 0) return null

        val track = try {
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
                        .setSampleRate(sampleRate)
                        .setChannelMask(channelMask)
                        .build(),
                )
                .setBufferSizeInBytes(minBufferBytes * 2)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
        } catch (_: Throwable) {
            return null
        }
        return AudioTrackRenderOutput(track)
    }
}

private class AudioTrackRenderOutput(private val track: AudioTrack) : RenderOutput {

    override fun play() = track.play()

    override fun write(buffer: FloatArray, offset: Int, size: Int): Int =
        track.write(buffer, offset, size, AudioTrack.WRITE_BLOCKING)

    override fun pause() = track.pause()

    override fun flush() = track.flush()

    override fun stop() = track.stop()

    override fun release() = track.release()
}
