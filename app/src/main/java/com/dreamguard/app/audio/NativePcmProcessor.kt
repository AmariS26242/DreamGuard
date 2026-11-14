package com.dreamguard.app.audio

import java.nio.ByteBuffer

/**
 * The JNI surface over the DG-022 PCM16 bridge (`pcm16_processor.h`).
 *
 * The native side owns the DG-013 compressor and the int16 <-> float conversion;
 * this object only loads the library and marshals direct ByteBuffers. It stays
 * free of Media3 types so it can be exercised by a contract test on the JVM.
 *
 * Handle lifetime: [create] returns an opaque `long` owned by the caller.
 * Exactly one [destroy] must eventually pair with it. All calls for one handle
 * must be serialised; [LocalAudioProcessor] does that by only touching the
 * handle on the playback thread.
 *
 * Failure policy: every entry point returns a negative [ERROR_*] code or 0 for
 * create instead of throwing. [LocalAudioProcessor] turns those into a
 * byte-exact pass-through and a user-visible status, so a missing or broken
 * library can never silence local playback.
 *
 * Tuning: the defaults are deliberately conservative and are the only parameters
 * the app uses. `makeup = -6.02 dB` keeps the loudness floor of the earlier
 * fixed -6 dB attenuation for quiet material; the compressor only adds gain
 * reduction above -18 dBFS. Attack/release are on the slow side so normal
 * programme material is not pumped. A linked -1 dBFS conservative 4x guard
 * follows compression, with 64-frame latency. The owner must call drain at
 * EOS/before bypass to retain the complete stream. Reset discards the tail.
 */
internal object NativePcmProcessor {

    private const val LIBRARY_NAME = "dreamguard_dsp"

    /** Fixed conservative defaults; see the class comment. */
    const val THRESHOLD_DB: Float = -18.0f
    const val RATIO: Float = 2.0f
    const val KNEE_DB: Float = 6.0f
    const val ATTACK_MS: Float = 20.0f
    const val RELEASE_MS: Float = 250.0f
    const val MAKEUP_GAIN_DB: Float = -6.0206f
    const val LINK_STEREO: Boolean = true

    /** Native error codes, mirrored from `jni_bridge.cpp`. */
    const val ERROR_INVALID_ARGUMENT: Int = -1
    const val ERROR_NULL_HANDLE: Int = -2
    const val ERROR_NOT_DIRECT: Int = -3
    const val ERROR_DSP: Int = -4

    /** Snapshot array indices packed by [nativeSnapshot]. */
    const val SNAPSHOT_FIELD_COUNT: Int = 16
    const val IDX_INPUT_MOMENTARY_LUFS: Int = 0
    const val IDX_INPUT_SHORT_TERM_LUFS: Int = 1
    const val IDX_OUTPUT_MOMENTARY_LUFS: Int = 2
    const val IDX_OUTPUT_SHORT_TERM_LUFS: Int = 3
    const val IDX_COMPRESSOR_GR_DB: Int = 4
    const val IDX_MAKEUP_GAIN_DB: Int = 5
    const val IDX_LOUDNESS_GAIN_DB: Int = 6
    const val IDX_LIMITER_GAIN_DB: Int = 7
    const val IDX_COMPLETED_HOPS: Int = 8
    const val IDX_LIMITER_LIMITED_SAMPLES: Int = 9
    const val IDX_INPUT_MOMENTARY_READY: Int = 10
    const val IDX_INPUT_SHORT_TERM_READY: Int = 11
    const val IDX_OUTPUT_MOMENTARY_READY: Int = 12
    const val IDX_OUTPUT_SHORT_TERM_READY: Int = 13
    const val IDX_LIMITER_ACTIVE: Int = 14
    const val IDX_LIMITER_ENABLED: Int = 15

    /**
     * True when `libdreamguard_dsp.so` loaded. Loaded lazily and only once;
     * a missing library is an expected state on the JVM and on unsupported ABIs,
     * not a crash.
     */
    val isAvailable: Boolean by lazy {
        runCatching { System.loadLibrary(LIBRARY_NAME) }.isSuccess
    }

    /** Returns a non-zero handle, or 0 if the library or the configuration failed. */
    fun create(sampleRate: Int, channelCount: Int): Long {
        if (!isAvailable) {
            return 0L
        }
        return nativeCreate(
            sampleRate,
            channelCount,
            THRESHOLD_DB,
            RATIO,
            KNEE_DB,
            ATTACK_MS,
            RELEASE_MS,
            MAKEUP_GAIN_DB,
            LINK_STEREO,
        )
    }

    /**
     * Processes `inputLength` bytes.
     *
     * @return the number of bytes written (equal to `inputLength` on success),
     *     or a negative [ERROR_*] code.
     */
    fun process(
        handle: Long,
        input: ByteBuffer,
        inputOffset: Int,
        inputLength: Int,
        output: ByteBuffer,
        outputOffset: Int,
        outputCapacity: Int,
    ): Int {
        if (!isAvailable) {
            return ERROR_NULL_HANDLE
        }
        return nativeProcess(
            handle,
            input,
            inputOffset,
            inputLength,
            output,
            outputOffset,
            outputCapacity,
        )
    }

    fun reset(handle: Long): Int = if (isAvailable) nativeReset(handle) else ERROR_NULL_HANDLE

    fun drain(handle: Long, output: ByteBuffer, outputOffset: Int): Int =
        if (isAvailable) nativeDrain(handle, output, outputOffset) else ERROR_NULL_HANDLE

    fun snapshot(handle: Long, outValues: DoubleArray): Int =
        if (isAvailable) nativeSnapshot(handle, outValues) else ERROR_NULL_HANDLE

    fun destroy(handle: Long): Int = if (isAvailable) nativeDestroy(handle) else ERROR_NULL_HANDLE

    fun version(): String = if (isAvailable) nativeVersion() else "native unavailable"

    // Private: an internal native method would get a Kotlin module suffix in its
    // JVM name, which no longer matches the exported C symbol. The contract test
    // checks this.
    private external fun nativeCreate(
        sampleRate: Int,
        channelCount: Int,
        thresholdDb: Float,
        ratio: Float,
        kneeDb: Float,
        attackMs: Float,
        releaseMs: Float,
        makeupGainDb: Float,
        linkStereo: Boolean,
    ): Long

    private external fun nativeProcess(
        handle: Long,
        input: ByteBuffer,
        inputOffset: Int,
        inputLength: Int,
        output: ByteBuffer,
        outputOffset: Int,
        outputCapacity: Int,
    ): Int

    private external fun nativeReset(handle: Long): Int

    private external fun nativeDrain(handle: Long, output: ByteBuffer, outputOffset: Int): Int

    private external fun nativeSnapshot(handle: Long, outValues: DoubleArray): Int

    private external fun nativeDestroy(handle: Long): Int

    private external fun nativeVersion(): String
}
