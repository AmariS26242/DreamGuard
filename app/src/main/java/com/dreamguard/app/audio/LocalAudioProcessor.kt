package com.dreamguard.app.audio

import androidx.media3.common.C
import androidx.media3.common.audio.AudioProcessor
import androidx.media3.common.audio.BaseAudioProcessor
import androidx.media3.common.util.UnstableApi
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Locale
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.roundToLong

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
 *  - Steady bypass copies bytes verbatim, including partial frames/odd bytes.
 *    Switching off first emits the retained tail and a bounded fade to dry.
 *  - Any native failure (library missing, handle creation failure, non-direct
 *    buffer, conversion error) also falls back to that byte-exact copy and sets
 *    [nativeState] / [nativeError] so the UI can explain what happened. Local
 *    playback must never go silent because of a DSP problem.
 *
 * The guard follows compression, has a -1 dBFS ceiling and retains 64 frames.
 * At EOS and before bypass the tail is emitted once. A cold processed stream
 * retains one 64-frame startup delay (1.333 ms at 48 kHz); after bypass has
 * emitted, later toggles strip unfilled startup zeros instead of adding delay.
 * The fade blends aligned dry/wet samples over 64 frames. Dry contributions
 * may exceed the DSP ceiling, as does ordinary bypass. Only fully wet output
 * has the native peak bound. Envelope weights are continuous on reversal;
 * first-derivative continuity and audible transparency are not guaranteed.
 * Physical listening is pending. The
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

    /** UI-facing classification of the bounded loudness/protection pipeline. */
    enum class ObservationState {
        /** Bypassed or unavailable: no loudness/compression measurement runs. */
        BYPASS,

        /** Active processing, but 400 ms momentary window is not ready yet. */
        OBSERVING,

        /** Window ready, and observed level is digital silence or below `-60 LUFS`. */
        SILENT_OR_LOW_LEVEL,

        /** Valid finite loudness observation for active audio. */
        ACTIVE,

        /** Audio processing continues, but telemetry failed or was invalid. */
        UNAVAILABLE,
    }

    /** Immutable throttled snapshot exposed to the playback service and UI. */
    data class LoudnessSnapshot(
        val state: ObservationState = ObservationState.BYPASS,
        val inputMomentaryLufs: Double = Double.NEGATIVE_INFINITY,
        val inputShortTermLufs: Double = Double.NEGATIVE_INFINITY,
        val outputMomentaryLufs: Double = Double.NEGATIVE_INFINITY,
        val outputShortTermLufs: Double = Double.NEGATIVE_INFINITY,
        val compressorGainReductionDb: Double = 0.0,
        val makeupGainDb: Double = 0.0,
        val loudnessGainDb: Double = 0.0,
        val limiterGainDb: Double = 0.0,
        val completedHops: Long = 0L,
        val limiterLimitedSamples: Long = 0L,
        val inputMomentaryReady: Boolean = false,
        val inputShortTermReady: Boolean = false,
        val outputMomentaryReady: Boolean = false,
        val outputShortTermReady: Boolean = false,
        val limiterActive: Boolean = false,
        val limiterEnabled: Boolean = false,
    ) {
        fun statusLabel(): String = when (state) {
            ObservationState.BYPASS -> "直通（未压缩）"
            ObservationState.OBSERVING -> "观测中（等待窗口就绪）"
            ObservationState.SILENT_OR_LOW_LEVEL -> "静音 / 极低电平"
            ObservationState.ACTIVE -> "实时观测"
            ObservationState.UNAVAILABLE -> "观测暂不可用"
        }

        fun inputMomentaryLabel(): String =
            formatLufs(state, inputMomentaryReady, inputMomentaryLufs)

        fun inputShortTermLabel(): String =
            formatLufs(state, inputShortTermReady, inputShortTermLufs)

        fun outputMomentaryLabel(): String =
            formatLufs(state, outputMomentaryReady, outputMomentaryLufs)

        fun outputShortTermLabel(): String =
            formatLufs(state, outputShortTermReady, outputShortTermLufs)

        fun gainSummaryLabel(): String = when (state) {
            ObservationState.BYPASS -> "直通"
            ObservationState.UNAVAILABLE -> "观测暂不可用"
            else -> String.format(
                Locale.US,
                "压缩 %+.1f dB · 补偿 %+.1f dB · 响度 %+.1f dB",
                compressorGainReductionDb,
                makeupGainDb,
                loudnessGainDb,
            )
        }

        fun protectionStatusLabel(): String = when {
            state == ObservationState.BYPASS -> "峰值保护：直通未启用"
            state == ObservationState.UNAVAILABLE -> "峰值保护：观测暂不可用"
            !limiterEnabled -> "峰值保护：未启用"
            limiterActive -> String.format(
                Locale.US,
                "峰值保护：限幅中 (%+.1f dB · %d 样点)",
                limiterGainDb,
                limiterLimitedSamples,
            )
            else -> "峰值保护：待命 ($limiterLimitedSamples 样点)"
        }
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

    // Transition coordination state (accessed exclusively on the playback thread).
    // Transition workspaces are pre-allocated. BaseAudioProcessor may allocate
    // its output buffer at first use or when a larger block arrives.
    private val drainScratch: ByteBuffer =
        ByteBuffer.allocateDirect(MAX_DRAIN_BYTES).order(ByteOrder.nativeOrder())
    private val inputLookaheadRing = ShortArray(LOOKAHEAD_FRAMES * MAX_NATIVE_CHANNELS)
    private val lastChannelGain = FloatArray(MAX_NATIVE_CHANNELS) { DEFAULT_PROCESSED_GAIN }
    private val fadeOutStartGain = FloatArray(MAX_NATIVE_CHANNELS) { DEFAULT_PROCESSED_GAIN }
    private var ringWritePos = 0
    private var primedFrames = 0
    private var hasEmittedBypassFrames = false
    private var suppressStartupDelay = false
    private var wasProcessingActive = false
    private var wetStep = 0
    private val snapshotScratch = DoubleArray(NativePcmProcessor.SNAPSHOT_FIELD_COUNT)
    private var framesSinceLastSnapshotPublish = 0L
    private var processedFramesSinceReset = 0L
    private var lastSnapshotState = ObservationState.BYPASS

    /** Number of audio blocks processed through the active native path since creation. */
    @Volatile
    var processedBlockCount: Long = 0L
        private set

    /** Number of playback-thread snapshot publications (throttled ~4 Hz + transitions). */
    @Volatile
    var publishedSnapshotCount: Long = 0L
        private set

    /** Latest bounded loudness & protection observation for UI display. */
    @Volatile
    var loudnessSnapshot: LoudnessSnapshot = LoudnessSnapshot()
        // A playback-thread read already in flight may publish after the UI disables processing.
        // Keep the visible state tied to the switch without touching native state on UI reads.
        get() = if (processingEnabled) field else BYPASS_SNAPSHOT
        private set

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
        if (!enabled) {
            loudnessSnapshot = LoudnessSnapshot()
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
        val enabled = processingEnabled

        if (!enabled) {
            handleBypassBlock(inputBuffer, inputPosition, remaining, output, outputPosition)
            publishBypassSnapshotIfNeeded()
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
                val outEnd = coordinateActiveBlock(
                    inputBuffer,
                    inputPosition,
                    remaining,
                    output,
                    outputPosition,
                )
                nativeTailPending = true
                inputBuffer.position(inputPosition + remaining)
                output.position(outEnd)
                output.flip()
                nativeState = NativeState.ACTIVE
                val activeFormat = nativeFormat ?: acceptedFormat
                val activeChannels =
                    (activeFormat?.channelCount ?: 1).coerceIn(MIN_NATIVE_CHANNELS, MAX_NATIVE_CHANNELS)
                val blockFrames = (remaining / (activeChannels * 2)).toLong()
                val sampleRate = activeFormat?.sampleRate ?: DEFAULT_OBSERVATION_SAMPLE_RATE
                recordActiveObservation(handle, blockFrames, sampleRate)
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
        val activeFormat = nativeFormat ?: acceptedFormat
        val fallbackFrameBytes = (activeFormat?.channelCount ?: 1) * 2
        resetTransitionState(bypassEmitted = remaining >= fallbackFrameBytes)
        publishBypassSnapshotIfNeeded()
        output.put(inputBuffer)
        output.flip()
        nativeState = if (nativeError != null) NativeState.ERROR else enabledState()
    }

    /** Set by [close]; no handle may be created after the owner has released the player. */
    @Volatile
    private var released: Boolean = false

    override fun onQueueEndOfStream() {
        if (nativeTailPending) {
            val output = replaceOutputBuffer(MAX_DRAIN_BYTES)
            val outEnd = emitDrainedTail(output, output.position(), fadeToBypass = false)
            output.position(outEnd)
            output.flip()
        }
        val handle = nativeHandle
        if (processingEnabled && handle != 0L && nativeError == null) readAndPublishSnapshot(handle)
    }

    private fun handleBypassBlock(
        inputBuffer: ByteBuffer,
        inputPosition: Int,
        remaining: Int,
        output: ByteBuffer,
        outputPosition: Int,
    ) {
        val channels = ((nativeFormat ?: acceptedFormat)?.channelCount ?: 1)
            .coerceIn(MIN_NATIVE_CHANNELS, MAX_NATIVE_CHANNELS)
        val frameBytes = channels * 2
        if (wasProcessingActive) {
            wasProcessingActive = false
            for (ch in 0 until channels) {
                fadeOutStartGain[ch] = lastChannelGain[ch]
            }
        }

        var outOffset = emitDrainedTail(output, outputPosition, fadeToBypass = true)

        val totalFrames = remaining / frameBytes
        if (totalFrames > 0) {
            hasEmittedBypassFrames = true
        }
        var frameIdx = 0
        while (wetStep > 0 && frameIdx < totalFrames) {
            wetStep--
            val alpha = WET_GAIN_TABLE[wetStep]
            val inFrameOffset = inputPosition + frameIdx * frameBytes
            for (ch in 0 until channels) {
                val dry = readPcm16(inputBuffer, inFrameOffset + ch * 2)
                val gain = (1.0f - alpha) + alpha * fadeOutStartGain[ch]
                val smoothed = clampPcm16((dry.toFloat() * gain).roundToInt())
                writePcm16(output, outOffset + ch * 2, smoothed)
            }
            outOffset += frameBytes
            frameIdx++
        }

        inputBuffer.position(inputPosition + frameIdx * frameBytes)
        output.position(outOffset)
        if (inputBuffer.hasRemaining()) {
            // Bit-exact bypass for all remaining frames and any odd trailing byte.
            output.put(inputBuffer)
        }
        output.flip()
    }

    private fun coordinateActiveBlock(
        inputBuffer: ByteBuffer,
        inputPosition: Int,
        remaining: Int,
        output: ByteBuffer,
        outputPosition: Int,
    ): Int {
        val channels = (nativeFormat?.channelCount ?: 1)
            .coerceIn(MIN_NATIVE_CHANNELS, MAX_NATIVE_CHANNELS)
        val frameBytes = channels * 2
        val latency = min(compressor.latencyFrames, LOOKAHEAD_FRAMES)
        val totalFrames = remaining / frameBytes
        val tailBytes = remaining - totalFrames * frameBytes

        if (!wasProcessingActive) {
            wasProcessingActive = true
            if (!hasEmittedBypassFrames && !nativeTailPending) {
                // Cold-start processed stream: preserve initial lookahead behavior and unity wet weight.
                suppressStartupDelay = false
                wetStep = TRANSITION_FRAMES
            } else if (!nativeTailPending && primedFrames == 0) {
                // Mid-stream OFF -> ON toggle: strip unfilled limiter zeros and ramp in smoothly.
                suppressStartupDelay = true
            }
        }

        if (latency == 0 && wetStep == TRANSITION_FRAMES) {
            updateGainTrackerDirect(inputBuffer, inputPosition, output, outputPosition, totalFrames, channels, frameBytes)
            return outputPosition + remaining
        }

        var writtenFrames = 0
        for (frameIdx in 0 until totalFrames) {
            val inByteOffset = inputPosition + frameIdx * frameBytes
            val wetByteOffset = outputPosition + frameIdx * frameBytes
            val outByteOffset = outputPosition + writtenFrames * frameBytes

            if (latency > 0 && primedFrames < latency) {
                val ringBase = ringWritePos * channels
                for (ch in 0 until channels) {
                    inputLookaheadRing[ringBase + ch] = readPcm16(inputBuffer, inByteOffset + ch * 2)
                }
                ringWritePos = (ringWritePos + 1) % latency
                primedFrames++
                if (!suppressStartupDelay) {
                    writtenFrames++
                }
                continue
            }

            if (wetStep < TRANSITION_FRAMES) {
                wetStep++
            }
            val alpha = WET_GAIN_TABLE[wetStep]
            val ringBase = ringWritePos * channels
            val needWrite = alpha < 1.0f || outByteOffset != wetByteOffset

            for (ch in 0 until channels) {
                val currentDry = readPcm16(inputBuffer, inByteOffset + ch * 2)
                val alignedDry = if (latency > 0) {
                    val delayed = inputLookaheadRing[ringBase + ch]
                    inputLookaheadRing[ringBase + ch] = currentDry
                    delayed
                } else {
                    currentDry
                }
                val wetSample = readPcm16(output, wetByteOffset + ch * 2)
                val dryAbs = abs(alignedDry.toInt())
                if (dryAbs >= GAIN_TRACK_MIN_SAMPLE) {
                    lastChannelGain[ch] =
                        (wetSample.toFloat() / alignedDry.toFloat()).coerceIn(0.0f, 1.0f)
                }
                if (needWrite) {
                    val sample = if (alpha >= 1.0f) wetSample else blendSamples(alignedDry, wetSample, alpha)
                    writePcm16(output, outByteOffset + ch * 2, sample)
                }
            }
            if (latency > 0) {
                ringWritePos = (ringWritePos + 1) % latency
            }
            writtenFrames++
        }

        val outBytes = writtenFrames * frameBytes + tailBytes
        if (tailBytes > 0 && outBytes != remaining) {
            val srcTailStart = outputPosition + totalFrames * frameBytes
            val dstTailStart = outputPosition + writtenFrames * frameBytes
            for (i in 0 until tailBytes) {
                output.put(dstTailStart + i, output.get(srcTailStart + i))
            }
        }
        return outputPosition + outBytes
    }

    private fun updateGainTrackerDirect(
        inputBuffer: ByteBuffer,
        inputPosition: Int,
        output: ByteBuffer,
        outputPosition: Int,
        totalFrames: Int,
        channels: Int,
        frameBytes: Int,
    ) {
        if (totalFrames <= 0) return
        val startFrame = maxOf(0, totalFrames - GAIN_TRACK_TAIL_FRAMES)
        for (frameIdx in startFrame until totalFrames) {
            val inOffset = inputPosition + frameIdx * frameBytes
            val wetOffset = outputPosition + frameIdx * frameBytes
            for (ch in 0 until channels) {
                val dry = readPcm16(inputBuffer, inOffset + ch * 2)
                if (abs(dry.toInt()) >= GAIN_TRACK_MIN_SAMPLE) {
                    val wet = readPcm16(output, wetOffset + ch * 2)
                    lastChannelGain[ch] = (wet.toFloat() / dry.toFloat()).coerceIn(0.0f, 1.0f)
                }
            }
        }
    }

    private fun emitDrainedTail(
        output: ByteBuffer,
        outputPosition: Int,
        fadeToBypass: Boolean,
    ): Int {
        val drainedBytes = drainNativeToScratch()
        if (drainedBytes <= 0) {
            if (fadeToBypass) {
                primedFrames = 0
                ringWritePos = 0
                suppressStartupDelay = false
            }
            return outputPosition
        }

        val channels = (nativeFormat?.channelCount ?: 1)
            .coerceIn(MIN_NATIVE_CHANNELS, MAX_NATIVE_CHANNELS)
        val frameBytes = channels * 2
        val latency = min(compressor.latencyFrames, LOOKAHEAD_FRAMES)
        var outOffset = outputPosition

        if (latency <= 0) {
            // Latency-free backends (or synthetic contract drains): preserve drained bytes verbatim.
            for (i in 0 until drainedBytes) {
                output.put(outOffset + i, drainScratch.get(i))
            }
            outOffset += drainedBytes
        } else {
            val totalDrainedFrames = drainedBytes / frameBytes
            val availableReal = min(totalDrainedFrames, primedFrames)
            val skipFrames = totalDrainedFrames - availableReal
            val ringReadStart = (ringWritePos - availableReal + latency) % latency

            if (!suppressStartupDelay && skipFrames > 0) {
                // Cold-start EOS or cold-start early toggle: keep unfilled startup frames.
                val skipBytes = skipFrames * frameBytes
                for (i in 0 until skipBytes) {
                    output.put(outOffset + i, drainScratch.get(i))
                }
                outOffset += skipBytes
            }

            for (k in 0 until availableReal) {
                val drainByteOffset = (skipFrames + k) * frameBytes
                val ringFrame = (ringReadStart + k) % latency
                if (fadeToBypass) {
                    if (wetStep > 0) {
                        wetStep--
                    }
                } else if (suppressStartupDelay) {
                    if (wetStep < TRANSITION_FRAMES) {
                        wetStep++
                    }
                }
                val alpha = WET_GAIN_TABLE[wetStep]
                for (ch in 0 until channels) {
                    val wetSample = readPcm16(drainScratch, drainByteOffset + ch * 2)
                    val drySample = inputLookaheadRing[ringFrame * channels + ch]
                    // A short cold stream can finish draining before its fade
                    // reaches dry. Continue from the gain of the actual last
                    // tail sample, not the default/stale pre-drain estimate.
                    if (fadeToBypass && abs(drySample.toInt()) >= GAIN_TRACK_MIN_SAMPLE) {
                        fadeOutStartGain[ch] =
                            (wetSample.toFloat() / drySample.toFloat()).coerceIn(0.0f, 1.0f)
                    }
                    val sample = if (alpha >= 1.0f) {
                        wetSample
                    } else if (alpha <= 0.0f) {
                        drySample
                    } else {
                        blendSamples(drySample, wetSample, alpha)
                    }
                    writePcm16(output, outOffset + ch * 2, sample)
                }
                outOffset += frameBytes
            }
            if (fadeToBypass && availableReal > 0) {
                hasEmittedBypassFrames = true
            }
        }

        primedFrames = 0
        ringWritePos = 0
        suppressStartupDelay = false
        return outOffset
    }

    private fun drainNativeToScratch(): Int {
        if (!nativeTailPending) return 0
        nativeTailPending = false
        if (nativeHandle == 0L || nativeError != null) return 0
        drainScratch.clear()
        val written = try {
            compressor.drain(nativeHandle, drainScratch, 0)
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

    private fun resetTransitionState(bypassEmitted: Boolean = false) {
        ringWritePos = 0
        primedFrames = 0
        hasEmittedBypassFrames = bypassEmitted
        suppressStartupDelay = false
        wasProcessingActive = false
        wetStep = 0
        for (ch in 0 until MAX_NATIVE_CHANNELS) {
            lastChannelGain[ch] = DEFAULT_PROCESSED_GAIN
            fadeOutStartGain[ch] = DEFAULT_PROCESSED_GAIN
        }
    }

    override fun onFlush() {
        nativeTailPending = false
        resetTransitionState()
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
        resetSnapshotTrackers()
        loudnessSnapshot = if (processingEnabled && nativeHandle != 0L && nativeError == null) {
            LoudnessSnapshot(state = ObservationState.OBSERVING, limiterEnabled = true)
        } else {
            LoudnessSnapshot()
        }
    }

    override fun onReset() {
        closeNative()
        acceptedFormat = null
        rejectedFormat = null
        nativeLayout = false
        nativeError = null
        nativeState = NativeState.OFF
        resetSnapshotTrackers()
        loudnessSnapshot = LoudnessSnapshot()
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
        resetSnapshotTrackers()
        loudnessSnapshot = LoudnessSnapshot()
    }

    private fun resetSnapshotTrackers() {
        framesSinceLastSnapshotPublish = 0L
        processedFramesSinceReset = 0L
        lastSnapshotState = ObservationState.BYPASS
    }

    private fun publishBypassSnapshotIfNeeded() {
        if (lastSnapshotState != ObservationState.BYPASS || loudnessSnapshot.state != ObservationState.BYPASS) {
            resetSnapshotTrackers()
            loudnessSnapshot = LoudnessSnapshot()
            publishedSnapshotCount++
        }
    }

    private fun recordActiveObservation(handle: Long, blockFrames: Long, sampleRate: Int) {
        processedBlockCount++
        val validFrames = max(0L, blockFrames)
        val framesBeforeBlock = processedFramesSinceReset
        framesSinceLastSnapshotPublish += validFrames
        processedFramesSinceReset += validFrames
        val rate = max(1, sampleRate).toLong()
        val publishIntervalFrames = max(1L, rate / SNAPSHOT_PUBLISH_HZ)
        val hopFrames = max(1L, (rate + LUFS_HOPS_PER_SECOND / 2) / LUFS_HOPS_PER_SECOND)
        val momentaryBoundaryReached =
            framesBeforeBlock < MOMENTARY_HOPS * hopFrames && processedFramesSinceReset >= MOMENTARY_HOPS * hopFrames
        val shortTermBoundaryReached =
            framesBeforeBlock < SHORT_TERM_HOPS * hopFrames && processedFramesSinceReset >= SHORT_TERM_HOPS * hopFrames
        val shouldPublish = lastSnapshotState == ObservationState.BYPASS ||
            framesSinceLastSnapshotPublish >= publishIntervalFrames ||
            momentaryBoundaryReached ||
            shortTermBoundaryReached
        if (shouldPublish) {
            readAndPublishSnapshot(handle)
        }
    }

    private fun readAndPublishSnapshot(handle: Long) {
        // Throttle attempts as well as successful publications. A telemetry
        // outage must not turn a display-rate read into one JNI call per block.
        framesSinceLastSnapshotPublish = 0L
        val status = try {
            compressor.snapshot(handle, snapshotScratch)
        } catch (_: Throwable) {
            NativePcmProcessor.ERROR_DSP
        }
        if (status != 0 || !validSnapshotPacket()) {
            if (lastSnapshotState != ObservationState.UNAVAILABLE) {
                loudnessSnapshot = LoudnessSnapshot(state = ObservationState.UNAVAILABLE)
                lastSnapshotState = ObservationState.UNAVAILABLE
                publishedSnapshotCount++
            }
            return
        }

        val inputMomentaryLufs = snapshotScratch[NativePcmProcessor.IDX_INPUT_MOMENTARY_LUFS]
        val inputShortTermLufs = snapshotScratch[NativePcmProcessor.IDX_INPUT_SHORT_TERM_LUFS]
        val outputMomentaryLufs = snapshotScratch[NativePcmProcessor.IDX_OUTPUT_MOMENTARY_LUFS]
        val outputShortTermLufs = snapshotScratch[NativePcmProcessor.IDX_OUTPUT_SHORT_TERM_LUFS]
        val compressorGrDb = snapshotScratch[NativePcmProcessor.IDX_COMPRESSOR_GR_DB]
        val makeupGainDb = snapshotScratch[NativePcmProcessor.IDX_MAKEUP_GAIN_DB]
        val loudnessGainDb = snapshotScratch[NativePcmProcessor.IDX_LOUDNESS_GAIN_DB]
        val limiterGainDb = snapshotScratch[NativePcmProcessor.IDX_LIMITER_GAIN_DB]
        val completedHops =
            snapshotScratch[NativePcmProcessor.IDX_COMPLETED_HOPS].roundToLong().coerceAtLeast(0L)
        val limiterLimitedSamples =
            snapshotScratch[NativePcmProcessor.IDX_LIMITER_LIMITED_SAMPLES].roundToLong()
                .coerceAtLeast(0L)
        val inputMomentaryReady =
            snapshotScratch[NativePcmProcessor.IDX_INPUT_MOMENTARY_READY] >= 0.5
        val inputShortTermReady =
            snapshotScratch[NativePcmProcessor.IDX_INPUT_SHORT_TERM_READY] >= 0.5
        val outputMomentaryReady =
            snapshotScratch[NativePcmProcessor.IDX_OUTPUT_MOMENTARY_READY] >= 0.5
        val outputShortTermReady =
            snapshotScratch[NativePcmProcessor.IDX_OUTPUT_SHORT_TERM_READY] >= 0.5
        val limiterActive = snapshotScratch[NativePcmProcessor.IDX_LIMITER_ACTIVE] >= 0.5
        val limiterEnabled = snapshotScratch[NativePcmProcessor.IDX_LIMITER_ENABLED] >= 0.5

        val anyReady = inputMomentaryReady || inputShortTermReady ||
            outputMomentaryReady || outputShortTermReady
        val inputLevel = if (inputShortTermReady) inputShortTermLufs else inputMomentaryLufs
        val outputLevel = if (outputShortTermReady) outputShortTermLufs else outputMomentaryLufs
        val inputLowOrSilent = !inputLevel.isFinite() || inputLevel <= LOW_LEVEL_FLOOR_LUFS
        val outputLowOrSilent = !outputLevel.isFinite() || outputLevel <= LOW_LEVEL_FLOOR_LUFS
        val nextState = when {
            !anyReady -> ObservationState.OBSERVING
            inputLowOrSilent && outputLowOrSilent -> ObservationState.SILENT_OR_LOW_LEVEL
            else -> ObservationState.ACTIVE
        }

        loudnessSnapshot = LoudnessSnapshot(
            state = nextState,
            inputMomentaryLufs = inputMomentaryLufs,
            inputShortTermLufs = inputShortTermLufs,
            outputMomentaryLufs = outputMomentaryLufs,
            outputShortTermLufs = outputShortTermLufs,
            compressorGainReductionDb = compressorGrDb,
            makeupGainDb = makeupGainDb,
            loudnessGainDb = loudnessGainDb,
            limiterGainDb = limiterGainDb,
            completedHops = completedHops,
            limiterLimitedSamples = limiterLimitedSamples,
            inputMomentaryReady = inputMomentaryReady,
            inputShortTermReady = inputShortTermReady,
            outputMomentaryReady = outputMomentaryReady,
            outputShortTermReady = outputShortTermReady,
            limiterActive = limiterActive,
            limiterEnabled = limiterEnabled,
        )
        lastSnapshotState = nextState
        framesSinceLastSnapshotPublish = 0L
        publishedSnapshotCount++
    }

    private fun validSnapshotPacket(): Boolean {
        for (i in 0..3) {
            val level = snapshotScratch[i]
            if (!level.isFinite() && level != Double.NEGATIVE_INFINITY) return false
        }
        for (i in 4..9) if (!snapshotScratch[i].isFinite()) return false
        for (i in 8..9) if (snapshotScratch[i] < 0.0) return false
        for (i in 10..15) if (snapshotScratch[i] != 0.0 && snapshotScratch[i] != 1.0) return false
        return true
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
        resetTransitionState()
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

        private val BYPASS_SNAPSHOT = LoudnessSnapshot()
        private const val MIN_NATIVE_CHANNELS = 1
        private const val MAX_NATIVE_CHANNELS = 2
        private const val LOOKAHEAD_FRAMES = 64
        private const val TRANSITION_FRAMES = 64
        private const val MAX_DRAIN_BYTES = LOOKAHEAD_FRAMES * MAX_NATIVE_CHANNELS * 2
        private const val DEFAULT_PROCESSED_GAIN = 0.5f
        private const val GAIN_TRACK_MIN_SAMPLE = 8
        private const val GAIN_TRACK_TAIL_FRAMES = 8
        private const val DEFAULT_OBSERVATION_SAMPLE_RATE = 48_000
        private const val SNAPSHOT_PUBLISH_HZ = 4L
        private const val LUFS_HOPS_PER_SECOND = 10L
        private const val MOMENTARY_HOPS = 4L
        private const val SHORT_TERM_HOPS = 30L
        private const val LOW_LEVEL_FLOOR_LUFS = -60.0
        private val WET_GAIN_TABLE = FloatArray(TRANSITION_FRAMES + 1) { step ->
            (0.5 * (1.0 - cos(Math.PI * step / TRANSITION_FRAMES))).toFloat()
        }
    }
}

private val IS_LITTLE_ENDIAN = ByteOrder.nativeOrder() == ByteOrder.LITTLE_ENDIAN

private fun readPcm16(buffer: ByteBuffer, byteOffset: Int): Short {
    val b0 = buffer.get(byteOffset).toInt() and 0xFF
    val b1 = buffer.get(byteOffset + 1).toInt() and 0xFF
    return if (IS_LITTLE_ENDIAN) {
        ((b1 shl 8) or b0).toShort()
    } else {
        ((b0 shl 8) or b1).toShort()
    }
}

private fun writePcm16(buffer: ByteBuffer, byteOffset: Int, value: Short) {
    val bits = value.toInt()
    if (IS_LITTLE_ENDIAN) {
        buffer.put(byteOffset, (bits and 0xFF).toByte())
        buffer.put(byteOffset + 1, ((bits ushr 8) and 0xFF).toByte())
    } else {
        buffer.put(byteOffset, ((bits ushr 8) and 0xFF).toByte())
        buffer.put(byteOffset + 1, (bits and 0xFF).toByte())
    }
}

private fun blendSamples(dry: Short, wet: Short, alpha: Float): Short {
    val value = dry.toFloat() * (1.0f - alpha) + wet.toFloat() * alpha
    return clampPcm16(value.roundToInt())
}

private fun clampPcm16(value: Int): Short =
    value.coerceIn(Short.MIN_VALUE.toInt(), Short.MAX_VALUE.toInt()).toShort()

private fun formatLufs(
    state: LocalAudioProcessor.ObservationState,
    ready: Boolean,
    value: Double,
): String {
    if (state == LocalAudioProcessor.ObservationState.BYPASS) return "直通"
    if (state == LocalAudioProcessor.ObservationState.UNAVAILABLE) return "暂不可用"
    if (!ready) return "观测中"
    if (!value.isFinite()) return if (value == Double.NEGATIVE_INFINITY) "静音" else "观测无效"
    if (value <= -60.0) return "极低 (≤ -60 LUFS)"
    return String.format(Locale.US, "%.1f LUFS", value)
}

private fun AudioProcessor.AudioFormat.isSupportedPcm16(): Boolean =
    encoding == C.ENCODING_PCM_16BIT && channelCount > 0 && sampleRate > 0
