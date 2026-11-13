package com.dreamguard.app.audio

import java.nio.ByteBuffer

/**
 * The PCM processing back end used by [LocalAudioProcessor].
 *
 * It exists so the Media3 processor can be tested on the JVM (where no `.so` can
 * load) without weakening the production path: the default implementation is
 * [JniPcmCompressor], and tests inject a recording fake.
 *
 * Contract:
 *  - [create] returns an opaque non-zero handle or 0; a handle is only ever used
 *    by the thread that created it.
 *  - [process] converts/compresses `inputLength` bytes from [input] into [output]
 *    and returns the number of bytes written, which must equal `inputLength` for
 *    the caller to accept the result. Any other value (including a negative
 *    error) makes the caller fall back to a byte-exact copy.
 *  - [reset] returns the compressor to unity gain but keeps the handle valid.
 *  - [destroy] releases the handle exactly once.
 *  - None of the methods throw across the interface; implementations catch JNI
 *    link errors themselves where they can, and the caller also wraps each call.
 */
internal interface PcmCompressor {
    val isAvailable: Boolean

    fun create(sampleRate: Int, channelCount: Int): Long

    fun process(
        handle: Long,
        input: ByteBuffer,
        inputOffset: Int,
        inputLength: Int,
        output: ByteBuffer,
        outputOffset: Int,
        outputCapacity: Int,
    ): Int

    /** Lookahead latency in frames retained by [process] and emitted by [drain]. */
    val latencyFrames: Int
        get() = 0

    fun reset(handle: Long)

    /** Emits retained PCM at EOS/before bypass. At most 256 bytes (64 stereo frames). */
    fun drain(handle: Long, output: ByteBuffer, outputOffset: Int): Int = 0

    fun destroy(handle: Long)
}

/** Production back end: the DG-022 JNI bridge. */
internal object JniPcmCompressor : PcmCompressor {

    override val isAvailable: Boolean
        get() = NativePcmProcessor.isAvailable

    override fun create(sampleRate: Int, channelCount: Int): Long =
        NativePcmProcessor.create(sampleRate, channelCount)

    override fun process(
        handle: Long,
        input: ByteBuffer,
        inputOffset: Int,
        inputLength: Int,
        output: ByteBuffer,
        outputOffset: Int,
        outputCapacity: Int,
    ): Int = NativePcmProcessor.process(
        handle,
        input,
        inputOffset,
        inputLength,
        output,
        outputOffset,
        outputCapacity,
    )

    override val latencyFrames: Int
        get() = 64

    override fun reset(handle: Long) {
        check(NativePcmProcessor.reset(handle) == 0) { "native reset failed" }
    }

    override fun drain(handle: Long, output: ByteBuffer, outputOffset: Int): Int =
        NativePcmProcessor.drain(handle, output, outputOffset)

    override fun destroy(handle: Long) {
        NativePcmProcessor.destroy(handle)
    }
}
