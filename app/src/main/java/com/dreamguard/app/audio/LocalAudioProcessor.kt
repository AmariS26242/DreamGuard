package com.dreamguard.app.audio

import androidx.media3.common.C
import androidx.media3.common.audio.AudioProcessor
import androidx.media3.common.audio.BaseAudioProcessor
import androidx.media3.common.util.UnstableApi
import java.nio.ByteBuffer

/**
 * The PCM stage on the Media3 playback path for **local** files, now backed by
 * the native compressor and conservative 4x peak guard.
 *
 * Scope and guarantees:
 *
 *  - Input: 16-bit PCM in the platform's native byte order, any sample rate.
 *    Mono and stereo (1–2 channels) go through the DG-022 bridge
 *    (`libdreamguard_dsp.so` → compressor → peak guard); wider layouts are accepted by
 *    negotiation but passed through byte for byte, because the compressor is a
 *    mono/stereo processor and silently down-mixing would be worse than saying
 *    "not supported".
 *  - Processing on: each block is converted to float, compressed with
 *    conservative defaults (threshold −18 dBFS, 2:1, 6 dB soft knee, 20 ms
 *    attack, 250 ms release, stereo linked) and converted back. The makeup gain
 *    is −6.02 dB, which keeps the old fixed −6 dB attenuation as the floor for
 *    quiet material while loud peaks are reduced further. That is why the switch
 *    is labelled "本地压缩" instead of "−6 dB": the audible contract changed from
 *    a constant gain to a level-dependent one, and the floor is unchanged.
 *  - Processing off (bypass): bytes are copied verbatim, so the output is
 *    bit-identical even for a partial frame or an odd trailing byte.
 *  - Any native failure (library missing, handle creation failure, non-direct
 *    buffer, conversion error) also falls back to that byte-exact copy and sets
 *    [nativeState] / [nativeError] so the UI can explain what happened. Local
 *    playback must never go silent because of a DSP problem.
 *
 * The guard follows compression, has a -1 dBFS ceiling and retains 64 frames.
 * At EOS and before bypass the tail is emitted once. Each processed segment
 * adds 64 silent startup frames (1.333 ms at 48 kHz); seeks discard old state.
 * Gain changes may still be audible; physical listening is pending. The
 * generative soundscape uses a separate handle and is unaffected by this stage.
 *
 * Threading and handles: [queueInput], [onConfigure], [onFlush] and [onReset]
 * run on the playback thread, and the native handle is created, used, reset and
 * destroyed only there — no lock, no per-sample allocation, no cross-thread
 * handle access. [setProcessingEnabled] may be called from any thread; it only
 * flips a `@Volatile` flag (read once per block) and refreshes the display
 * state.
 *
 * Lifecycle follows Media3's contract: [onConfigure] only records a *pending*
 * format, and the handle is replaced in [onFlush], where the new format is
 * actually applied. Between the two Media3 may still queue buffers in the old
 * format, and those keep using the old handle. A handle is therefore never
 * destroyed while it may still be processing the stream it was created for; a
 * same-format flush (the normal seek case) keeps the handle and only resets the
 * compressor.
 */
@UnstableApi
class LocalAudioProcessor internal constructor(
    private val compressor: PcmCompressor,
) : BaseAudioProcessor() {

    /** Production processor: the JNI bridge, or a byte-exact bypass without it. */
    constructor() : this(JniPcmCompressor)

    /** What the local PCM stage is currently doing; used by the UI and tests. */
    enum class NativeState {
        /** Processing is switched off: bytes are copied verbatim. */
        OFF,

        /** Enabled, the negotiated format is supported and no failure has occurred yet. */
        READY,

        /** The native compressor processed the most recent block. */
        ACTIVE,

        /** The negotiated format is not PCM16; the sink bypasses this stage. */
        UNSUPPORTED_FORMAT,

        /** PCM16 with more than two channels: passed through verbatim. */
        UNSUPPORTED_LAYOUT,

        /** `libdreamguard_dsp.so` could not be loaded: passed through verbatim. */
        NO_NATIVE_LIBRARY,

        /** Native create/process failed: passed through verbatim. */
        ERROR,
    }

    /** Whether PCM is currently allowed to be transformed. Volatile. */
    @Volatile
    var processingEnabled: Boolean = false
        private set

    /** Last format accepted by [onConfigure], for diagnostics/tests. */
    @Volatile
    var acceptedFormat: AudioProcessor.AudioFormat? = null
        private set

    /** Last format rejected by [onConfigure], for the UI's format note. */
    @Volatile
    var rejectedFormat: AudioProcessor.AudioFormat? = null
        private set

    /** Current path of the stage; see [NativeState]. */
    @Volatile
    var nativeState: NativeState = NativeState.OFF
        private set

    /** Last native failure, or null. Shown by the UI when [nativeState] is ERROR. */
    @Volatile
    var nativeError: String? = null
        private set

    /** Native handle for the applied format; 0 when none exists. */
    @Volatile
    private var nativeHandle: Long = 0L

    /** Format [nativeHandle] was created for, or null when there is no handle. */
    @Volatile
    private var nativeFormat: AudioProcessor.AudioFormat? = null

    // Accessed only on the playback thread. A switch-off still owes the sink
    // the end of the preceding processed block before its byte-exact bypass.
    private var nativeTailPending = false

    /** Whether the negotiated layout is one the compressor supports (1–2 ch). */
    @Volatile
    private var nativeLayout: Boolean = false

    /** Whether the negotiated channel layout is one the compressor supports. */
    val layoutSupported: Boolean
        get() = nativeLayout

    /** Whether `libdreamguard_dsp.so` could be loaded (cached after first use). */
    val nativeLibraryAvailable: Boolean
        get() = compressor.isAvailable

    /** Last native failure, or null. [nativeState] is ERROR while this is set. */
    val nativeFailure: String?
        get() = nativeError

    /**
     * Whether turning processing on can actually process the negotiated format.
     * A transient processing failure clears it until the next `flush()` retries;
     * the UI uses it to explain the state, not to trap the switch.
     */
    val nativeCapable: Boolean
        get() = nativeLayout && compressor.isAvailable && nativeError == null

    fun setProcessingEnabled(enabled: Boolean) {
        processingEnabled = enabled
        nativeState = if (enabled) {
            enabledState()
        } else if (rejectedFormat != null) {
            NativeState.UNSUPPORTED_FORMAT
        } else {
            NativeState.OFF
        }
    }

    override fun onConfigure(inputAudioFormat: AudioProcessor.AudioFormat): AudioProcessor.AudioFormat {
        if (released) {
            // The service already tore this stage down; stay inactive and let the
            // sink pass the format through untouched.
            return AudioProcessor.AudioFormat.NOT_SET
        }
        // Media3 may still queue buffers in the old format between configure() and
        // flush(), so nothing that the old stream depends on is touched here: the
        // live handle is only replaced in onFlush(), where the new format is
        // actually applied. This is also why a configure() that is never flushed
        // cannot close a handle that is still being used.
        if (!inputAudioFormat.isSupportedPcm16()) {
            acceptedFormat = null
            rejectedFormat = inputAudioFormat
            nativeLayout = false
            nativeState = NativeState.UNSUPPORTED_FORMAT
            // Inactive: the sink bypasses the processor for this format.
            return AudioProcessor.AudioFormat.NOT_SET
        }

        acceptedFormat = inputAudioFormat
        rejectedFormat = null
        nativeLayout = inputAudioFormat.channelCount in MIN_NATIVE_CHANNELS..MAX_NATIVE_CHANNELS
        nativeState = when {
            !processingEnabled -> NativeState.OFF
            !nativeLayout -> NativeState.UNSUPPORTED_LAYOUT
            !compressor.isAvailable -> NativeState.NO_NATIVE_LIBRARY
            nativeError != null -> NativeState.ERROR
            else -> NativeState.READY
        }
        // Same rate, channel count and encoding: this stage is format-neutral.
        return inputAudioFormat
    }

    override fun queueInput(inputBuffer: ByteBuffer) {
        if (!inputBuffer.hasRemaining()) {
            return
        }
        val remaining = inputBuffer.remaining()
        val output = replaceOutputBuffer(remaining + MAX_DRAIN_BYTES)
        val inputPosition = inputBuffer.position()
        val outputPosition = output.position()

        if (!processingEnabled) {
            output.position(drainNative(output))
            // Bit-exact bypass: copy every byte, including a possible odd tail.
            output.put(inputBuffer)
            output.flip()
            nativeState = if (rejectedFormat != null) {
                NativeState.UNSUPPORTED_FORMAT
            } else {
                NativeState.OFF
            }
            return
        }

        val handle = nativeHandle
        if (handle != 0L && nativeError == null) {
            val written = try {
                compressor.process(
                    handle,
                    inputBuffer,
                    inputPosition,
                    remaining,
                    output,
                    outputPosition,
                    output.capacity(),
                )
            } catch (error: Throwable) {
                nativeError = "native process threw ${error.javaClass.simpleName}"
                -1
            }
            if (written == remaining) {
                nativeTailPending = true
                inputBuffer.position(inputPosition + remaining)
                output.position(outputPosition + written)
                output.flip()
                nativeState = NativeState.ACTIVE
                return
            }
            // The native call may have written part of the block before failing;
            // rewind both buffers and let the byte copy below produce a complete
            // output block. Rewinding the input too keeps the fallback exact even
            // if a backend consumed part of it before reporting the failure.
            output.position(outputPosition)
            inputBuffer.position(inputPosition)
            if (nativeError == null) {
                nativeError = if (written >= 0) {
                    "native process wrote $written of $remaining bytes"
                } else {
                    "native process failed ($written)"
                }
            }
        }

        // Safe fallback: identical bytes, no matter why the native path is gone.
        nativeTailPending = false
        output.put(inputBuffer)
        output.flip()
        nativeState = if (nativeError != null) NativeState.ERROR else enabledState()
    }

    /** Set by [close]; no handle may be created after the owner has released the player. */
    @Volatile
    private var released: Boolean = false

    override fun onQueueEndOfStream() {
        if (!nativeTailPending) return
        val output = replaceOutputBuffer(MAX_DRAIN_BYTES)
        output.position(drainNative(output))
        output.flip()
    }

    private fun drainNative(output: ByteBuffer): Int {
        if (!nativeTailPending) return 0
        nativeTailPending = false
        if (nativeHandle == 0L || nativeError != null) return 0
        val written = try {
            compressor.drain(nativeHandle, output, 0)
        } catch (error: Throwable) {
            nativeError = "native drain threw ${error.javaClass.simpleName}"
            nativeState = NativeState.ERROR
            return 0
        }
        val frameBytes = (nativeFormat?.channelCount ?: 1) * 2
        if (written !in 0..MAX_DRAIN_BYTES || written % frameBytes != 0) {
            nativeError = "native drain failed ($written)"
            nativeState = NativeState.ERROR
            return 0
        }
        return written
    }

    override fun onFlush() {
        nativeTailPending = false
        if (released) {
            // A sink that resets its processors after the owner released the player
            // must not resurrect a native handle that nothing will use or free.
            closeNative()
            nativeState = NativeState.OFF
            return
        }
        // BaseAudioProcessor.flush() has already cleared any pending output and
        // copied the *pending* format into inputAudioFormat, so this is the point
        // where the stage really switches formats. Creating the handle here (not
        // in onConfigure) is what keeps a configure()/flush() pair safe while the
        // sink still had old-format buffers queued: those kept using the old
        // handle until this call.
        val format = inputAudioFormat
        // A flush is a stream boundary: a transient processing failure from the old
        // stream must not leak into the newly applied configuration.
        nativeError = null
        if (!format.isSupportedPcm16() ||
            format.channelCount !in MIN_NATIVE_CHANNELS..MAX_NATIVE_CHANNELS ||
            !compressor.isAvailable
        ) {
            closeNative()
        } else if (nativeHandle != 0L && nativeFormat == format) {
            // Same applied format (the normal seek/restart case): keep the handle,
            // clear the compressor state, and retry the native path after a
            // transient processing failure.
            try {
                compressor.reset(nativeHandle)
            } catch (error: Throwable) {
                nativeError = "native reset threw ${error.javaClass.simpleName}"
                closeNative()
            }
        } else {
            // First application of this format, or a real format change.
            closeNative()
            createNative(format)
        }
        nativeState = when {
            rejectedFormat != null -> NativeState.UNSUPPORTED_FORMAT
            !processingEnabled -> NativeState.OFF
            else -> enabledState()
        }
    }

    override fun onReset() {
        closeNative()
        acceptedFormat = null
        rejectedFormat = null
        nativeLayout = false
        nativeError = null
        nativeState = NativeState.OFF
    }

    /**
     * Releases the native handle at service teardown. Call only after the player
     * that owns this processor has been released, so no playback thread can be
     * inside [queueInput] any more; idempotent. Media3 has no "close" hook on
     * [AudioProcessor], and not every sink guarantees a final reset().
     */
    fun close() {
        released = true
        closeNative()
        // Terminal: a late UI callback must not switch native processing back on
        // after the service has released the player, and nothing may report the
        // stage as capable any more.
        acceptedFormat = null
        rejectedFormat = null
        nativeLayout = false
        processingEnabled = false
        nativeError = null
        nativeState = NativeState.OFF
    }

    private fun createNative(format: AudioProcessor.AudioFormat) {
        val handle = try {
            compressor.create(format.sampleRate, format.channelCount)
        } catch (error: Throwable) {
            nativeError = "native create threw ${error.javaClass.simpleName}"
            0L
        }
        if (handle == 0L) {
            if (nativeError == null) {
                nativeError = "native create failed"
            }
            return
        }
        nativeHandle = handle
        nativeFormat = format
    }

    private fun closeNative() {
        nativeTailPending = false
        val handle = nativeHandle
        nativeHandle = 0L
        nativeFormat = null
        if (handle == 0L) {
            return
        }
        try {
            compressor.destroy(handle)
        } catch (_: Throwable) {
            // Teardown must not throw; the handle is already forgotten.
        }
    }

    private fun enabledState(): NativeState = when {
        rejectedFormat != null -> NativeState.UNSUPPORTED_FORMAT
        acceptedFormat == null -> NativeState.OFF
        !nativeLayout -> NativeState.UNSUPPORTED_LAYOUT
        !compressor.isAvailable -> NativeState.NO_NATIVE_LIBRARY
        nativeError != null -> NativeState.ERROR
        else -> NativeState.READY
    }

    companion object {
        /** UI label for the processing switch: a level-dependent compressor. */
        const val GAIN_LABEL: String = "本地压缩"

        /** Longest explanation the format note adds for the current state. */
        const val BYPASS_LABEL: String = "直通"

        private const val MIN_NATIVE_CHANNELS = 1
        private const val MAX_NATIVE_CHANNELS = 2
        private const val MAX_DRAIN_BYTES = 64 * MAX_NATIVE_CHANNELS * 2
    }
}

private fun AudioProcessor.AudioFormat.isSupportedPcm16(): Boolean =
    encoding == C.ENCODING_PCM_16BIT && channelCount > 0 && sampleRate > 0
