package com.dreamguard.app.debug

import android.app.Activity
import android.app.Instrumentation
import android.os.Bundle
import androidx.media3.common.C
import androidx.media3.common.audio.AudioProcessor
import androidx.media3.common.util.UnstableApi
import com.dreamguard.app.audio.LocalAudioProcessor
import com.dreamguard.app.audio.NativePcmProcessor
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.abs
import kotlin.math.sin

/**
 * Runs the real Kotlin/JNI/native chain without audio routing or microphone use.
 * adb shell am instrument -w com.dreamguard.app/.debug.AudioTransitionCheck
 * This verifies PCM results, not physical playback or audible transparency.
 */
@UnstableApi
class AudioTransitionCheck : Instrumentation() {
    override fun onCreate(arguments: Bundle?) {
        super.onCreate(arguments)
        start()
    }

    override fun onStart() {
        val result = Bundle()
        try {
            check(NativePcmProcessor.isAvailable) { "Native library unavailable" }
            var cases = 0
            var worstColdBoundary = 0
            for (rate in intArrayOf(8000, 44100, 48000, 96000, 192000)) {
                for (channels in 1..2) {
                    val tone = input(1024, channels) { frame, ch ->
                        (24000 * sin(2 * Math.PI * 997 * frame / rate + ch * 0.3)).toInt()
                    }
                    val direct = renderNative(tone, rate, channels)
                    val coordinated = render(tone, rate, channels, listOf(1024 to true), 7)
                    check(direct.contentEquals(coordinated)) { "Always-on mismatch $rate/$channels" }
                    cases++

                    val dc = input(2048, channels) { _, ch -> if (ch == 0) 30000 else -15000 }
                    for (period in intArrayOf(1, 16, 63, 64, 65, 128)) {
                        val segments = buildList {
                            var remaining = 2048
                            var enabled = false
                            while (remaining > 0) {
                                val take = minOf(remaining, period)
                                add(take to enabled)
                                remaining -= take
                                enabled = !enabled
                            }
                        }
                        val whole = render(dc, rate, channels, segments, 2048)
                        val split = render(dc, rate, channels, segments, 7)
                        check(whole.size == dc.size && whole.contentEquals(split)) {
                            "Count/chunk mismatch $rate/$channels period=$period"
                        }
                        val pcm = ByteBuffer.wrap(whole).order(ByteOrder.nativeOrder())
                        while (pcm.hasRemaining()) check(pcm.short.toInt() != 0) {
                            "Inserted silence $rate/$channels period=$period"
                        }
                        cases++
                    }
                    for (active in intArrayOf(1, 16, 63)) {
                        val cold = input(active + 192, channels) { _, ch -> if (ch == 0) 30000 else -15000 }
                        val output = render(cold, rate, channels, listOf(active to true, 192 to false), 7)
                        check(output.size == cold.size + 64 * channels * 2) { "Cold startup count" }
                        val pcm = ByteBuffer.wrap(output).order(ByteOrder.nativeOrder())
                        val boundary = active + 64
                        for (ch in 0 until channels) {
                            val step = abs(pcm.getShort((boundary * channels + ch) * 2).toInt() -
                                pcm.getShort(((boundary - 1) * channels + ch) * 2).toInt())
                            worstColdBoundary = maxOf(worstColdBoundary, step)
                            check(step <= 1000) { "Cold drain step $step at $rate/$channels/$active" }
                        }
                        val dryBytes = 64 * channels * 2
                        check(output.copyOfRange(output.size - dryBytes, output.size)
                            .contentEquals(cold.copyOfRange(cold.size - dryBytes, cold.size))) {
                            "Steady bypass changed"
                        }
                        cases++
                    }
                }
            }
            result.putString("stream", "PASS $cases real JNI cases; worst cold boundary=$worstColdBoundary LSB\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            result.putString("stream", "FAIL ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }

    private fun input(frames: Int, channels: Int, sample: (Int, Int) -> Int): ByteArray {
        val pcm = ByteBuffer.allocate(frames * channels * 2).order(ByteOrder.nativeOrder())
        repeat(frames) { frame -> repeat(channels) { ch -> pcm.putShort(sample(frame, ch).toShort()) } }
        return pcm.array()
    }

    private fun ByteArrayOutputStream.consume(buffer: ByteBuffer) {
        val bytes = ByteArray(buffer.remaining())
        buffer.get(bytes)
        write(bytes)
    }

    private fun render(
        input: ByteArray,
        rate: Int,
        channels: Int,
        segments: List<Pair<Int, Boolean>>,
        maxChunk: Int,
    ): ByteArray {
        val processor = LocalAudioProcessor()
        try {
            processor.configure(AudioProcessor.AudioFormat(rate, channels, C.ENCODING_PCM_16BIT))
            processor.flush()
            check(processor.nativeCapable) { processor.nativeFailure ?: "Unsupported format" }
            val source = ByteBuffer.allocateDirect(input.size).order(ByteOrder.nativeOrder())
            source.put(input).flip()
            val result = ByteArrayOutputStream(input.size + 256)
            var cursor = 0
            for ((frames, enabled) in segments) {
                processor.setProcessingEnabled(enabled)
                var remaining = frames
                while (remaining > 0) {
                    val take = minOf(remaining, maxChunk)
                    source.limit((cursor + take) * channels * 2)
                    source.position(cursor * channels * 2)
                    processor.queueInput(source)
                    check(processor.nativeFailure == null) { processor.nativeFailure ?: "Processing error" }
                    result.consume(processor.output)
                    cursor += take
                    remaining -= take
                }
            }
            check(cursor * channels * 2 == input.size)
            processor.queueEndOfStream()
            check(processor.nativeFailure == null) { processor.nativeFailure ?: "Drain error" }
            result.consume(processor.output)
            check(processor.isEnded)
            return result.toByteArray()
        } finally {
            processor.close()
        }
    }

    private fun renderNative(input: ByteArray, rate: Int, channels: Int): ByteArray {
        val handle = NativePcmProcessor.create(rate, channels)
        check(handle != 0L)
        try {
            val source = ByteBuffer.allocateDirect(input.size).order(ByteOrder.nativeOrder())
            source.put(input).flip()
            val output = ByteBuffer.allocateDirect(input.size).order(ByteOrder.nativeOrder())
            check(NativePcmProcessor.process(handle, source, 0, input.size, output, 0, output.capacity()) == input.size)
            val result = ByteArrayOutputStream(input.size + 256)
            output.limit(input.size)
            result.consume(output)
            val tail = ByteBuffer.allocateDirect(256).order(ByteOrder.nativeOrder())
            val written = NativePcmProcessor.drain(handle, tail, 0)
            check(written == 64 * channels * 2)
            tail.limit(written)
            result.consume(tail)
            return result.toByteArray()
        } finally {
            NativePcmProcessor.destroy(handle)
        }
    }
}
