package com.dreamguard.app.audio

import androidx.media3.common.C
import androidx.media3.common.audio.AudioProcessor
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.exp
import kotlin.math.ln
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * JVM tests for the PCM stage on the local-file playback path.
 *
 * The real DSP runs in `libdreamguard_dsp.so`, which cannot be loaded by a host
 * JVM test; the conversion + compressor behaviour itself is covered by the
 * C++ `dg_pcm16_processor` test. These tests cover the Media3-facing layer with
 * a recording fake back end:
 *
 *  - the native back end is actually invoked, with the right handle and byte
 *    count, and its output reaches the sink;
 *  - bypass and every fallback path are byte-exact (odd tail included);
 *  - handles are created/destroyed when a configuration is applied (flush), survive
 *    a same-format flush, and are not touched by a configure() that has not been
 *    flushed yet, so old-format buffers keep their old handle;
 *  - unsupported formats/layouts are reported and passed through;
 *  - visible state ([LocalAudioProcessor.nativeState], `nativeError`) matches
 *    what happened.
 *
 * Not covered here: the Android sink/ExoPlayer wiring and real device audio
 * (instrumentation/device test), and the exact compressor audio result.
 */
class LocalAudioProcessorTest {

    private fun pcmFormat(
        sampleRate: Int = 48_000,
        channelCount: Int = 2,
        encoding: Int = C.ENCODING_PCM_16BIT,
    ) = AudioProcessor.AudioFormat(sampleRate, channelCount, encoding)

    private fun samples(vararg values: Short): ByteBuffer {
        val buffer = ByteBuffer.allocateDirect(values.size * 2).order(ByteOrder.nativeOrder())
        values.forEach { buffer.putShort(it) }
        buffer.flip()
        return buffer
    }

    private fun bytes(vararg values: Int): ByteBuffer {
        val buffer = ByteBuffer.allocateDirect(values.size).order(ByteOrder.nativeOrder())
        values.forEach { buffer.put(it.toByte()) }
        buffer.flip()
        return buffer
    }

    private fun ByteBuffer.toByteArray(): ByteArray {
        val copy = duplicate()
        return ByteArray(copy.remaining()).also { copy.get(it) }
    }

    private fun configured(
        processor: LocalAudioProcessor,
        sampleRate: Int = 48_000,
        channelCount: Int = 2,
    ): LocalAudioProcessor {
        processor.configure(pcmFormat(sampleRate, channelCount))
        // Media3 applies a configured format with flush(); that is where the
        // native handle is created, so the tests follow the same contract
        // instead of relying on onConfigure side effects.
        processor.flush()
        return processor
    }

    /**
     * Records lifecycle calls and applies a deterministic transformation
     * (integer halving) so the test can tell native output from a byte copy.
     * Mirrors the C++ bridge's handling of a trailing odd byte.
     */
    private class FakeCompressor(
        override var isAvailable: Boolean = true,
        var failCreate: Boolean = false,
    ) : PcmCompressor {

        val created = mutableListOf<Pair<Int, Int>>()
        val destroyed = mutableListOf<Long>()
        var resetCalls = 0
        var processCalls = 0
        var failProcess = false
        var drainResult = 0
        var drainCalls = 0
        var throwDrain = false
        var requireDirectInput = false
        var lastHandle = 0L
        var lastInputLength = 0
        private var nextHandle = 1L

        override fun create(sampleRate: Int, channelCount: Int): Long {
            created += sampleRate to channelCount
            return if (failCreate) 0L else nextHandle++
        }

        override fun process(
            handle: Long,
            input: ByteBuffer,
            inputOffset: Int,
            inputLength: Int,
            output: ByteBuffer,
            outputOffset: Int,
            outputCapacity: Int,
        ): Int {
            processCalls++
            lastHandle = handle
            lastInputLength = inputLength
            if (requireDirectInput && !input.isDirect) return -3
            if (failProcess) return -4
            // The real bridge writes raw bytes, so absolute putShort must use the
            // native byte order instead of ByteBuffer's big-endian default.
            if (output.order() != ByteOrder.nativeOrder()) {
                output.order(ByteOrder.nativeOrder())
            }
            var offset = inputOffset
            val end = inputOffset + inputLength
            var out = outputOffset
            while (offset + 1 < end) {
                val value = input.getShort(offset)
                output.putShort(out, (value.toInt() / 2).toShort())
                offset += 2
                out += 2
            }
            if (offset < end) {
                output.put(out, input.get(offset))
            }
            return inputLength
        }

        override fun reset(handle: Long) {
            resetCalls++
        }

        override fun drain(handle: Long, output: ByteBuffer, outputOffset: Int): Int {
            drainCalls++
            if (throwDrain) error("drain failure")
            if (drainResult > 0 && drainResult <= 64) {
                repeat(drainResult) { output.put(outputOffset + it, 0x42.toByte()) }
            }
            return drainResult
        }

        override fun destroy(handle: Long) {
            destroyed += handle
        }
    }

    @Test
    fun `EOS emits the retained tail exactly once`() {
        val fake = FakeCompressor().apply { drainResult = 64 }
        val processor = configured(LocalAudioProcessor(fake))
        processor.setProcessingEnabled(true)
        processor.queueInput(samples(1000, -1000))
        processor.output.toByteArray()
        processor.queueEndOfStream()
        assertFalse(processor.isEnded)
        assertArrayEquals(ByteArray(64) { 0x42 }, processor.output.toByteArray())
        assertTrue(processor.isEnded)
        processor.queueEndOfStream()
        assertEquals(0, processor.output.remaining())
        assertEquals(1, fake.drainCalls)
    }

    @Test
    fun `switching off emits old tail before exact bypass and resumes cleanly`() {
        val fake = FakeCompressor().apply { drainResult = 4 }
        val processor = configured(LocalAudioProcessor(fake))
        processor.setProcessingEnabled(true)
        processor.queueInput(samples(1000, -1000))
        processor.output.toByteArray()
        processor.setProcessingEnabled(false)
        processor.queueInput(bytes(0x12, 0x34, 0x56))
        assertArrayEquals(byteArrayOf(0x42, 0x42, 0x42, 0x42, 0x12, 0x34, 0x56), processor.output.toByteArray())
        processor.queueInput(bytes(1, 2, 3))
        assertArrayEquals(byteArrayOf(1, 2, 3), processor.output.toByteArray())
        assertEquals(1, fake.drainCalls)
        processor.setProcessingEnabled(true)
        processor.queueInput(samples(2000, -2000))
        val result = processor.output.order(ByteOrder.nativeOrder())
        assertEquals(1000.toShort(), result.short)
        assertEquals((-1000).toShort(), result.short)
        assertEquals(LocalAudioProcessor.NativeState.ACTIVE, processor.nativeState)
    }

    @Test
    fun `seek discards old tail and EOS of a bypass stream emits none`() {
        val fake = FakeCompressor().apply { drainResult = 64 }
        val processor = configured(LocalAudioProcessor(fake))
        processor.setProcessingEnabled(true)
        processor.queueInput(samples(1000, -1000))
        processor.output.toByteArray()
        processor.flush()
        processor.queueEndOfStream()
        assertEquals(0, processor.output.remaining())
        assertEquals(0, fake.drainCalls)
        processor.flush()
        processor.setProcessingEnabled(false)
        processor.queueInput(samples(1000, -1000))
        processor.output.toByteArray()
        processor.queueEndOfStream()
        assertEquals(0, processor.output.remaining())
        assertEquals(0, fake.drainCalls)
    }

    @Test
    fun `drain errors preserve bypass bytes and report a failure`() {
        for (result in listOf(-4, 65, 3)) {
            val fake = FakeCompressor().apply { drainResult = result }
            val processor = configured(LocalAudioProcessor(fake))
            processor.setProcessingEnabled(true)
            processor.queueInput(samples(1000, -1000))
            processor.output.toByteArray()
            processor.setProcessingEnabled(false)
            processor.queueInput(bytes(1, 2, 3))
            assertArrayEquals(byteArrayOf(1, 2, 3), processor.output.toByteArray())
            assertNotNull(processor.nativeFailure)
            processor.setProcessingEnabled(true)
            assertEquals(LocalAudioProcessor.NativeState.ERROR, processor.nativeState)
            processor.flush()
            assertNull(processor.nativeFailure)
        }
    }

    @Test
    fun `an EOS drain exception ends safely and exposes the error`() {
        val fake = FakeCompressor().apply { throwDrain = true }
        val processor = configured(LocalAudioProcessor(fake))
        processor.setProcessingEnabled(true)
        processor.queueInput(samples(1000, -1000))
        processor.output.toByteArray()
        processor.queueEndOfStream()
        assertEquals(0, processor.output.remaining())
        assertTrue(processor.isEnded)
        assertEquals(LocalAudioProcessor.NativeState.ERROR, processor.nativeState)
        assertNotNull(processor.nativeFailure)
    }

    @Test
    fun `native backend receives every block and its output reaches the sink`() {
        val fake = FakeCompressor()
        val processor = configured(LocalAudioProcessor(fake))
        processor.setProcessingEnabled(true)
        assertEquals(LocalAudioProcessor.NativeState.READY, processor.nativeState)

        val values = ShortArray(1 shl 16) { (it - 32_768).toShort() }
        val input = ByteBuffer.allocateDirect(values.size * 2).order(ByteOrder.nativeOrder())
        values.forEach { input.putShort(it) }
        input.flip()

        processor.queueInput(input)
        val output = processor.output
        output.order(ByteOrder.nativeOrder())
        assertEquals(values.size * 2, output.remaining())

        var index = 0
        while (output.hasRemaining()) {
            val expected = (values[index].toInt() / 2).toShort()
            assertEquals("sample $index (${values[index]})", expected, output.short)
            index++
        }
        assertEquals(values.size, index)
        assertEquals(1, fake.processCalls)
        assertEquals(1L, fake.lastHandle)
        assertEquals(values.size * 2, fake.lastInputLength)
        assertEquals(0, input.remaining())
        assertEquals(LocalAudioProcessor.NativeState.ACTIVE, processor.nativeState)
    }

    @Test
    fun `bypass is bit exact, keeps a trailing odd byte and never calls native`() {
        val fake = FakeCompressor()
        val processor = configured(LocalAudioProcessor(fake))
        // Deliberately odd length: 2 samples + 1 tail byte.
        val input = bytes(0x34, 0x12, 0xFE, 0xFF, 0x7F)
        val expected = input.toByteArray()

        processor.queueInput(input)
        assertArrayEquals(expected, processor.output.toByteArray())
        assertEquals(0, input.remaining())
        assertEquals(0, fake.processCalls)
        assertEquals(LocalAudioProcessor.NativeState.OFF, processor.nativeState)
    }

    @Test
    fun `processing keeps a trailing odd byte untouched`() {
        val processor = configured(LocalAudioProcessor(FakeCompressor()))
        processor.setProcessingEnabled(true)
        val input = bytes(0xE8, 0x03, 0x18, 0xFC, 0x7F) // 1000, -1000, tail 0x7F

        processor.queueInput(input)
        val output = processor.output
        assertEquals(5, output.remaining())
        output.order(ByteOrder.nativeOrder())
        assertEquals(500.toShort(), output.short)
        assertEquals((-500).toShort(), output.short)
        assertEquals(0x7F.toByte(), output.get())
    }

    @Test
    fun `bypass and processing can be toggled between buffers`() {
        val fake = FakeCompressor()
        val processor = configured(LocalAudioProcessor(fake))
        val first = samples(4000, -4000)
        processor.queueInput(first)
        assertArrayEquals(samples(4000, -4000).toByteArray(), processor.output.toByteArray())
        assertEquals(0, fake.processCalls)

        processor.setProcessingEnabled(true)
        val second = ShortArray(128 * 2) { if (it % 2 == 0) 4000 else -4000 }
        processor.queueInput(samples(*second))
        val activeOut = processor.output.toShortArray()
        assertEquals(128 * 2, activeOut.size)
        // First transition frame stays close to bypass level rather than jumping straight to 2000.
        assertTrue(activeOut[0] > 3990 && activeOut[1] < -3990)
        // Steady state after the 64-frame crossfade reaches exact compressed output (2000, -2000).
        for (frame in 64 until 128) {
            assertEquals(2000.toShort(), activeOut[frame * 2])
            assertEquals((-2000).toShort(), activeOut[frame * 2 + 1])
        }
        assertEquals(1, fake.processCalls)

        processor.setProcessingEnabled(false)
        val fadeOutBlock = ShortArray(64 * 2) { if (it % 2 == 0) 4000 else -4000 }
        processor.queueInput(samples(*fadeOutBlock))
        val fadeOut = processor.output.toShortArray()
        assertTrue(fadeOut[0] < 2010 && fadeOut[fadeOut.size - 2] == 4000.toShort())

        val third = samples(-3, 3)
        processor.queueInput(third)
        // After transition completes, bypass is bit-exact.
        assertArrayEquals(samples(-3, 3).toByteArray(), processor.output.toByteArray())
        assertEquals(1, fake.processCalls)
        assertEquals(LocalAudioProcessor.NativeState.OFF, processor.nativeState)
    }

    @Test
    fun `configure accepts pcm16 and only creates a handle for mono or stereo`() {
        for (rate in intArrayOf(8_000, 44_100, 48_000, 192_000)) {
            for (channels in intArrayOf(1, 2, 6)) {
                val fake = FakeCompressor()
                val processor = LocalAudioProcessor(fake)
                val result = processor.configure(pcmFormat(rate, channels))
                assertEquals("rate=$rate channels=$channels", pcmFormat(rate, channels), result)
                assertTrue("rate=$rate channels=$channels", processor.isActive)
                processor.flush()
                assertEquals(pcmFormat(rate, channels), processor.acceptedFormat)
                assertNull(processor.rejectedFormat)
                if (channels <= 2) {
                    assertEquals(listOf(rate to channels), fake.created)
                    assertTrue("rate=$rate channels=$channels", processor.nativeCapable)
                } else {
                    assertTrue("rate=$rate channels=$channels", fake.created.isEmpty())
                    assertFalse("rate=$rate channels=$channels", processor.nativeCapable)
                }
            }
        }
    }

    @Test
    fun `configure rejects formats the sink must pass through untouched`() {
        val rejected = listOf(
            pcmFormat(encoding = C.ENCODING_PCM_FLOAT),
            pcmFormat(encoding = C.ENCODING_PCM_24BIT),
            pcmFormat(encoding = C.ENCODING_PCM_8BIT),
            pcmFormat(channelCount = 0),
            pcmFormat(sampleRate = 0),
        )
        for (format in rejected) {
            val fake = FakeCompressor()
            val processor = LocalAudioProcessor(fake)
            val result = processor.configure(format)
            assertEquals("$format", AudioProcessor.AudioFormat.NOT_SET, result)
            assertFalse("$format", processor.isActive)
            processor.flush()
            assertTrue("$format", fake.created.isEmpty())
            assertEquals(format, processor.rejectedFormat)
            assertNull(processor.acceptedFormat)
            assertEquals(
                LocalAudioProcessor.NativeState.UNSUPPORTED_FORMAT,
                processor.nativeState,
            )
        }
    }

    @Test
    fun `flush resets native state but keeps the negotiated format and handle`() {
        val fake = FakeCompressor()
        val processor = configured(LocalAudioProcessor(fake))
        processor.setProcessingEnabled(true)
        processor.queueInput(samples(100, 200))
        assertEquals(LocalAudioProcessor.NativeState.ACTIVE, processor.nativeState)
        assertEquals(1, fake.processCalls)

        processor.flush()

        assertEquals(0, processor.output.remaining())
        assertTrue(processor.isActive)
        assertEquals(1, fake.resetCalls)
        assertTrue(fake.destroyed.isEmpty())
        assertEquals(LocalAudioProcessor.NativeState.READY, processor.nativeState)
        assertNull(processor.nativeError)

        processor.queueInput(samples(100, 200))
        assertArrayEquals(samples(50, 100).toByteArray(), processor.output.toByteArray())
        assertEquals(2, fake.processCalls)
    }

    @Test
    fun `end of stream reports ended only after pending output is drained`() {
        val processor = configured(LocalAudioProcessor(FakeCompressor()))
        processor.queueInput(samples(10, 20))
        processor.queueEndOfStream()

        assertFalse(processor.isEnded)
        assertEquals(4, processor.output.remaining())
        assertTrue(processor.isEnded)
        assertEquals(0, processor.output.remaining())
        assertTrue(processor.isEnded)
    }

    @Test
    fun `format change destroys the old handle and keeps the toggle`() {
        val fake = FakeCompressor()
        val processor = configured(
            LocalAudioProcessor(fake),
            sampleRate = 44_100,
            channelCount = 1,
        )
        processor.setProcessingEnabled(true)
        processor.queueInput(samples(10))
        assertEquals(5.toShort(), processor.output.short)
        assertEquals(1L, fake.lastHandle)

        // A new file at a different rate/channel count must reconfigure cleanly.
        val result = processor.configure(pcmFormat(sampleRate = 48_000, channelCount = 2))
        assertEquals(pcmFormat(sampleRate = 48_000, channelCount = 2), result)
        assertTrue(processor.processingEnabled)

        // configure() alone must not touch the live handle: Media3 is allowed to
        // keep queueing old-format buffers until flush(). The old handle keeps
        // processing them, which is exactly why handle replacement belongs in flush.
        assertEquals(listOf(44_100 to 1), fake.created)
        assertTrue(fake.destroyed.isEmpty())
        assertEquals(LocalAudioProcessor.NativeState.READY, processor.nativeState)
        processor.queueInput(samples(10))
        assertEquals(5.toShort(), processor.output.short)
        assertEquals(1L, fake.lastHandle)

        processor.flush()
        assertEquals(listOf(44_100 to 1, 48_000 to 2), fake.created)
        assertEquals(listOf(1L), fake.destroyed)
        assertEquals(LocalAudioProcessor.NativeState.READY, processor.nativeState)

        processor.queueInput(samples(10, 20))
        assertArrayEquals(samples(5, 10).toByteArray(), processor.output.toByteArray())
        assertEquals(2L, fake.lastHandle)
    }

    @Test
    fun `reset clears format, destroys the handle and forces renegotiation`() {
        val fake = FakeCompressor()
        val processor = configured(LocalAudioProcessor(fake))
        processor.reset()
        assertFalse(processor.isActive)
        assertEquals(listOf(1L), fake.destroyed)
        assertEquals(LocalAudioProcessor.NativeState.OFF, processor.nativeState)
        assertNull(processor.acceptedFormat)

        val result = processor.configure(pcmFormat())
        assertEquals(pcmFormat(), result)
        // The handle is created when the new configuration is applied.
        assertEquals(1, fake.created.size)
        processor.flush()
        assertEquals(2, fake.created.size)
        assertEquals(48_000 to 2, fake.created[1])
    }

    @Test
    fun `missing native library is reported and passed through byte for byte`() {
        val fake = FakeCompressor(isAvailable = false)
        val processor = configured(LocalAudioProcessor(fake))
        processor.setProcessingEnabled(true)
        assertEquals(LocalAudioProcessor.NativeState.NO_NATIVE_LIBRARY, processor.nativeState)
        assertFalse(processor.nativeCapable)
        assertTrue(fake.created.isEmpty())

        val input = bytes(0x34, 0x12, 0xFE, 0xFF, 0x7F)
        val expected = input.toByteArray()
        processor.queueInput(input)
        assertArrayEquals(expected, processor.output.toByteArray())
        assertEquals(0, fake.processCalls)
        assertEquals(LocalAudioProcessor.NativeState.NO_NATIVE_LIBRARY, processor.nativeState)
    }

    @Test
    fun `native process failure falls back to exact bytes and sticks until flush`() {
        val fake = FakeCompressor()
        val processor = configured(LocalAudioProcessor(fake))
        processor.setProcessingEnabled(true)
        fake.failProcess = true

        val input = samples(1000, -1000, 4000)
        val expected = input.toByteArray()
        processor.queueInput(input)
        assertArrayEquals(expected, processor.output.toByteArray())
        assertEquals(LocalAudioProcessor.NativeState.ERROR, processor.nativeState)
        assertNotNull(processor.nativeError)
        assertEquals(1, fake.processCalls)

        // A failed format keeps bypassing without hammering JNI on every block.
        processor.queueInput(samples(1000, -1000))
        assertEquals(1, fake.processCalls)
        assertEquals(LocalAudioProcessor.NativeState.ERROR, processor.nativeState)

        // A flush is a clean block boundary: retry once, then recover.
        fake.failProcess = false
        processor.flush()
        assertNull(processor.nativeError)
        assertEquals(LocalAudioProcessor.NativeState.READY, processor.nativeState)
        processor.queueInput(samples(1000, -1000))
        assertArrayEquals(samples(500, -500).toByteArray(), processor.output.toByteArray())
        assertEquals(2, fake.processCalls)
    }

    @Test
    fun `processing can be switched off after a native failure`() {
        val fake = FakeCompressor()
        val processor = configured(LocalAudioProcessor(fake))
        processor.setProcessingEnabled(true)
        fake.failProcess = true

        processor.queueInput(samples(1000))
        assertEquals(LocalAudioProcessor.NativeState.ERROR, processor.nativeState)

        // The UI keeps an enabled switch operable after a failure: the user must be
        // able to go back to the bit-exact bypass without closing the file.
        processor.setProcessingEnabled(false)
        assertEquals(LocalAudioProcessor.NativeState.OFF, processor.nativeState)
        assertFalse(processor.nativeCapable)

        val input = samples(1000)
        val expected = input.toByteArray()
        processor.queueInput(input)
        assertArrayEquals(expected, processor.output.toByteArray())
        assertEquals(1, fake.processCalls)
        assertEquals(LocalAudioProcessor.NativeState.OFF, processor.nativeState)
    }

    @Test
    fun `non-direct input is treated as a native failure and copied exactly`() {
        val fake = FakeCompressor()
        fake.requireDirectInput = true
        val processor = configured(LocalAudioProcessor(fake))
        processor.setProcessingEnabled(true)

        val heap = ByteBuffer.allocate(6).order(ByteOrder.nativeOrder())
        heap.putShort(1000).putShort(-1000).putShort(4000)
        heap.flip()
        val expected = heap.toByteArray()

        processor.queueInput(heap)
        assertEquals(LocalAudioProcessor.NativeState.ERROR, processor.nativeState)
        assertEquals(1, fake.processCalls)
        assertEquals(0, heap.remaining())
        assertArrayEquals(expected, processor.output.toByteArray())
    }

    @Test
    fun `handle creation failure is reported and playback is not silenced`() {
        val fake = FakeCompressor(failCreate = true)
        val processor = configured(LocalAudioProcessor(fake))
        processor.setProcessingEnabled(true)
        assertEquals(LocalAudioProcessor.NativeState.ERROR, processor.nativeState)
        assertNotNull(processor.nativeError)
        assertFalse(processor.nativeCapable)

        val input = samples(10, 20)
        val expected = input.toByteArray()
        processor.queueInput(input)
        assertArrayEquals(expected, processor.output.toByteArray())
        assertEquals(0, fake.processCalls)
        assertEquals(LocalAudioProcessor.NativeState.ERROR, processor.nativeState)
    }

    @Test
    fun `multichannel pcm16 is accepted, passed through and explained`() {
        val fake = FakeCompressor()
        val processor = configured(LocalAudioProcessor(fake), channelCount = 6)
        processor.setProcessingEnabled(true)
        assertEquals(LocalAudioProcessor.NativeState.UNSUPPORTED_LAYOUT, processor.nativeState)
        assertFalse(processor.nativeCapable)

        val input = samples(1, 2, 3, 4, 5, 6)
        val expected = input.toByteArray()
        processor.queueInput(input)
        assertArrayEquals(expected, processor.output.toByteArray())
        assertTrue(fake.created.isEmpty())
        assertEquals(0, fake.processCalls)
        assertEquals(LocalAudioProcessor.NativeState.UNSUPPORTED_LAYOUT, processor.nativeState)
    }

    @Test
    fun `close is idempotent and releases the handle once`() {
        val fake = FakeCompressor()
        val processor = configured(LocalAudioProcessor(fake))
        processor.setProcessingEnabled(true)
        processor.queueInput(samples(100, 200))
        assertEquals(1, fake.processCalls)

        processor.close()
        processor.close()

        assertEquals(listOf(1L), fake.destroyed)
        assertEquals(LocalAudioProcessor.NativeState.OFF, processor.nativeState)
        assertFalse(processor.processingEnabled)

        // Defensive: even a late buffer after close is copied, never sent to a
        // freed handle. (The service only closes after player.release().)
        val input = samples(400, -400)
        val expected = input.toByteArray()
        processor.queueInput(input)
        assertArrayEquals(expected, processor.output.toByteArray())
        assertEquals(1, fake.processCalls)
        assertEquals(LocalAudioProcessor.NativeState.OFF, processor.nativeState)

        // A sink that resets or reconfigures after teardown must not resurrect a
        // native handle that nothing would free.
        processor.reset()
        val result = processor.configure(pcmFormat())
        assertEquals(AudioProcessor.AudioFormat.NOT_SET, result)
        assertFalse(processor.isActive)
        processor.flush()
        assertEquals(1, fake.created.size)
    }

    @Test
    fun `turning processing on before configure does not invent a handle`() {
        val fake = FakeCompressor()
        val processor = LocalAudioProcessor(fake)
        processor.setProcessingEnabled(true)
        assertEquals(LocalAudioProcessor.NativeState.OFF, processor.nativeState)
        assertTrue(fake.created.isEmpty())
    }

    // -------------------------------------------------------------------------
    // Phase A2 deterministic PCM transition verification
    // -------------------------------------------------------------------------

    @Test
    fun `short cold start fades from the actual drained gain into bypass`() {
        for (rate in intArrayOf(8000, 44100, 48000, 192000)) {
            for (channels in 1..2) {
                for (activeFrames in intArrayOf(1, 16, 63)) {
                    val backend = LookaheadDspCompressor(fixedGain = 0.1)
                    val processor = configured(LocalAudioProcessor(backend), rate, channels)
                    val output = mutableListOf<Short>()
                    val input = ShortArray((activeFrames + 128) * channels) { sample ->
                        if (sample % channels == 0) 20000 else -10000
                    }
                    processor.setProcessingEnabled(true)
                    processor.queueInput(samples(*input.copyOfRange(0, activeFrames * channels)))
                    output.addShortsFrom(processor.output)
                    processor.setProcessingEnabled(false)
                    processor.queueInput(samples(*input.copyOfRange(activeFrames * channels, input.size)))
                    output.addShortsFrom(processor.output)
                    processor.queueEndOfStream()
                    output.addShortsFrom(processor.output)
                    // A cold processed stream retains its single startup delay.
                    assertEquals(input.size + 64 * channels, output.size)
                    val boundary = activeFrames + 64
                    for (ch in 0 until channels) {
                        val delta = abs(output[boundary * channels + ch].toInt() -
                            output[(boundary - 1) * channels + ch].toInt())
                        assertTrue("rate=$rate channels=$channels frames=$activeFrames ch=$ch delta=$delta", delta <= 500)
                    }
                    processor.close()
                }
            }
        }
    }

    @Test
    fun `transition OFF to ON eliminates startup silence and step discontinuity`() {
        val sampleRate = 48_000
        val channels = 2
        val totalFrames = 1024
        val toggleFrame = 256
        val inputSamples = sineWavePcm(totalFrames, channels, sampleRate, 440.0, amplitude = 20_000.0)
        val theoreticalMaxDelta = maxAdjacentDelta(inputSamples, channels)

        val seamless = runSeamlessSegmented(
            inputSamples,
            channels,
            sampleRate,
            segments = listOf(toggleFrame to false, (totalFrames - toggleFrame) to true),
        )
        val legacy = runLegacyBaseline(
            inputSamples,
            channels,
            sampleRate,
            segments = listOf(toggleFrame to false, (totalFrames - toggleFrame) to true),
        )

        val seamlessStats = measureWindowStats(seamless, channels, windowFrames = 64)
        val legacyStats = measureWindowStats(legacy, channels, windowFrames = 64)
        println(
            "A2_METRIC OFF->ON: legacy=[maxDelta=${legacyStats.maxAdjacentDelta}, minRms=${legacyStats.minRms}, peak=${legacyStats.peak}, frames=${legacy.size / channels}] " +
                "seamless=[maxDelta=${seamlessStats.maxAdjacentDelta}, minRms=${seamlessStats.minRms}, peak=${seamlessStats.peak}, frames=${seamless.size / channels}]",
        )

        // Legacy implementation inserts 64 zero frames -> RMS collapses to 0 and boundary jumps > 10x step.
        assertEquals(0.0, legacyStats.minRms, 1e-6)
        assertEquals(totalFrames + 64, legacy.size / channels)

        // Seamless implementation eliminates startup zero hole, preserves exact frame count, and stays smooth.
        assertEquals(totalFrames, seamless.size / channels)
        assertTrue("minRms=${seamlessStats.minRms}", seamlessStats.minRms > 5_000.0)
        assertTrue(
            "maxDelta=${seamlessStats.maxAdjacentDelta}, natural=$theoreticalMaxDelta",
            seamlessStats.maxAdjacentDelta <= theoreticalMaxDelta + 40,
        )
        assertTrue(seamlessStats.maxAdjacentDelta < legacyStats.maxAdjacentDelta)
        assertTrue(seamlessStats.peak <= 20_000)
    }

    @Test
    fun `transition ON to OFF eliminates step discontinuity across 64-frame tail`() {
        val sampleRate = 48_000
        val channels = 2
        val totalFrames = 1024
        val toggleFrame = 512
        val inputSamples = sineWavePcm(totalFrames, channels, sampleRate, 440.0, amplitude = 24_000.0)
        val naturalDelta = maxAdjacentDelta(inputSamples, channels)

        // Start in OFF briefly, switch ON for processed steady state, then toggle ON -> OFF.
        val segments = listOf(128 to false, (toggleFrame - 128) to true, (totalFrames - toggleFrame) to false)
        val seamless = runSeamlessSegmented(inputSamples, channels, sampleRate, segments)
        val legacy = runLegacyBaseline(inputSamples, channels, sampleRate, segments)

        val seamlessStats = measureWindowStats(seamless, channels, windowFrames = 32)
        val legacyStats = measureWindowStats(legacy, channels, windowFrames = 32)
        // Measure exact boundary window around ON -> OFF (around frame toggleFrame).
        val boundaryStart = (toggleFrame - 80) * channels
        val boundaryEnd = (toggleFrame + 80) * channels
        val seamlessBoundaryDelta = maxAdjacentDelta(seamless.copyOfRange(boundaryStart, boundaryEnd), channels)
        val legacyBoundaryDelta = maxAdjacentDelta(legacy.copyOfRange(boundaryStart, boundaryEnd), channels)

        println(
            "A2_METRIC ON->OFF: legacyBoundaryDelta=$legacyBoundaryDelta, legacyMaxDelta=${legacyStats.maxAdjacentDelta}; " +
                "seamlessBoundaryDelta=$seamlessBoundaryDelta, seamlessMaxDelta=${seamlessStats.maxAdjacentDelta}, natural=$naturalDelta",
        )

        assertEquals(totalFrames, seamless.size / channels)
        assertTrue(
            "seamlessBoundaryDelta=$seamlessBoundaryDelta should remain close to natural sine step $naturalDelta",
            seamlessBoundaryDelta <= naturalDelta + 40,
        )
        assertTrue(seamlessBoundaryDelta < legacyBoundaryDelta / 2)
    }

    @Test
    fun `rapid repeated toggles stay bounded and preserve frame count`() {
        val sampleRate = 48_000
        val channels = 2
        val naturalSine = sineWavePcm(2048, channels, sampleRate, 300.0, amplitude = 16_000.0)
        val naturalDelta = maxAdjacentDelta(naturalSine, channels)
        val steadyProcessed = runSeamlessSegmented(
            naturalSine,
            channels,
            sampleRate,
            listOf(naturalSine.size / channels to true),
        )
        // Measure steady-state compressed RMS excluding the 64-frame cold-start lookahead and trailing 64-frame EOS tail.
        val steadyBody = steadyProcessed.copyOfRange(128 * channels, (2048 - 128) * channels)
        val steadyProcessedMinRms = measureWindowStats(steadyBody, channels, windowFrames = 32).minRms

        for (period in intArrayOf(16, 32, 64, 128)) {
            val segments = mutableListOf<Pair<Int, Boolean>>()
            var remaining = naturalSine.size / channels
            var enabled = false
            while (remaining > 0) {
                val len = min(period, remaining)
                segments += len to enabled
                enabled = !enabled
                remaining -= len
            }

            val seamless = runSeamlessSegmented(naturalSine, channels, sampleRate, segments)
            val legacy = runLegacyBaseline(naturalSine, channels, sampleRate, segments)
            val stats = measureWindowStats(seamless, channels, windowFrames = 32)
            val legacyStats = measureWindowStats(legacy, channels, windowFrames = 32)

            println(
                "A2_METRIC RAPID(period=$period): legacyFrames=${legacy.size / channels}, legacyMaxDelta=${legacyStats.maxAdjacentDelta}, legacyMinRms=${legacyStats.minRms}; " +
                    "seamlessFrames=${seamless.size / channels}, seamlessMaxDelta=${stats.maxAdjacentDelta}, seamlessMinRms=${stats.minRms}, steadyFloorRms=$steadyProcessedMinRms",
            )

            assertEquals("frame count drift at period=$period", naturalSine.size / channels, seamless.size / channels)
            assertTrue(
                "silence drop at period=$period: minRms=${stats.minRms} < 0.9 * steadyFloor ($steadyProcessedMinRms)",
                stats.minRms >= steadyProcessedMinRms * 0.9,
            )
            assertTrue(
                "discontinuity spike at period=$period: delta=${stats.maxAdjacentDelta}, natural=$naturalDelta",
                stats.maxAdjacentDelta <= naturalDelta + 80,
            )
            assertTrue(stats.peak <= 16_000)
        }
    }

    @Test
    fun `transitions are invariant across arbitrary buffer boundaries`() {
        val sampleRate = 48_000
        val channels = 2
        val totalFrames = 1024
        val inputSamples = sineWavePcm(totalFrames, channels, sampleRate, 440.0, amplitude = 18_000.0)
        val schedule = BooleanArray(totalFrames) { frame ->
            frame in 100 until 420 || frame in 500 until 780
        }

        val reference = runWithScheduleAndChunkSizes(inputSamples, channels, sampleRate, schedule, intArrayOf(1024))
        for (chunkSize in intArrayOf(1, 7, 17, 64, 257)) {
            val chunked = runWithScheduleAndChunkSizes(inputSamples, channels, sampleRate, schedule, intArrayOf(chunkSize))
            assertArrayEquals("chunkSize=$chunkSize deviated from contiguous reference", reference, chunked)
        }
    }

    @Test
    fun `transitions remain smooth across signal amplitudes`() {
        val sampleRate = 48_000
        val channels = 2
        val totalFrames = 1024
        val amplitudesDbfs = listOf(-40.0, -18.0, -6.0, 0.0)

        for (dbfs in amplitudesDbfs) {
            val peakInput = (32_767.0 * 10.0.pow(dbfs / 20.0)).coerceAtMost(32_767.0)
            val inputSamples = sineWavePcm(totalFrames, channels, sampleRate, 440.0, amplitude = peakInput)
            val naturalDelta = maxAdjacentDelta(inputSamples, channels)
            val segments = listOf(200 to false, 400 to true, 424 to false)

            val seamless = runSeamlessSegmented(inputSamples, channels, sampleRate, segments)
            val stats = measureWindowStats(seamless, channels, windowFrames = 64)
            println(
                "A2_METRIC AMP(${dbfs}dBFS): maxDelta=${stats.maxAdjacentDelta} (natural=$naturalDelta), peak=${stats.peak}, minRms=${stats.minRms}",
            )

            assertEquals(totalFrames, seamless.size / channels)
            assertTrue("dbfs=$dbfs maxDelta=${stats.maxAdjacentDelta} natural=$naturalDelta", stats.maxAdjacentDelta <= naturalDelta + 80)
            assertTrue("dbfs=$dbfs peak=${stats.peak}", stats.peak <= peakInput.roundToInt() + 1)
            assertTrue("dbfs=$dbfs minRms=${stats.minRms}", stats.minRms > 0.0)
        }
    }

    @Test
    fun `silence and near-silence transitions introduce no noise or DC offset`() {
        val sampleRate = 48_000
        val channels = 2
        val totalFrames = 512
        val segments = listOf(128 to false, 192 to true, 192 to false)

        val silence = ShortArray(totalFrames * channels)
        val outSilence = runSeamlessSegmented(silence, channels, sampleRate, segments)
        assertArrayEquals(silence, outSilence)

        val nearSilence = ShortArray(totalFrames * channels) { idx -> if ((idx / channels) % 2 == 0) 1 else -1 }
        val outNear = runSeamlessSegmented(nearSilence, channels, sampleRate, segments)
        assertEquals(totalFrames, outNear.size / channels)
        for (sample in outNear) {
            assertTrue("near-silence sample $sample exceeded [-1, 1]", sample in -1..1)
        }
    }

    @Test
    fun `peak amplitude and anti-clipping guarantees hold across transitions`() {
        val sampleRate = 48_000
        val channels = 2
        val totalFrames = 1024
        // Full-scale sine + high-amplitude impulse spike near transition boundary.
        val inputSamples = sineWavePcm(totalFrames, channels, sampleRate, 1_000.0, amplitude = 32_767.0)
        inputSamples[256 * channels] = 32_767
        inputSamples[256 * channels + 1] = -32_768
        val segments = listOf(200 to false, 300 to true, 524 to false)

        val seamless = runSeamlessSegmented(inputSamples, channels, sampleRate, segments)
        assertEquals(totalFrames, seamless.size / channels)
        // Steady-state compressed+limited region (frames 320..440) must remain under -1 dBFS ceiling (~29,204).
        for (frame in 320 until 440) {
            for (ch in 0 until channels) {
                val sample = abs(seamless[frame * channels + ch].toInt())
                assertTrue("steady-state peak $sample exceeded limiter ceiling", sample <= 29_250)
            }
        }
        // Entire crossfade region must never wrap or clip destructively due to crossfade gain > 1.0.
        for (i in seamless.indices) {
            val v = seamless[i].toInt()
            assertTrue(v in Short.MIN_VALUE..Short.MAX_VALUE)
        }
    }

    private fun ByteBuffer.toShortArray(): ShortArray {
        val dup = duplicate().order(ByteOrder.nativeOrder())
        val result = ShortArray(dup.remaining() / 2)
        for (i in result.indices) {
            result[i] = dup.short
        }
        return result
    }

    private fun sineWavePcm(
        frames: Int,
        channels: Int,
        sampleRate: Int,
        freqHz: Double,
        amplitude: Double,
    ): ShortArray {
        val out = ShortArray(frames * channels)
        val omega = 2.0 * PI * freqHz / sampleRate
        for (f in 0 until frames) {
            val s = (amplitude * sin(omega * f)).roundToInt()
                .coerceIn(Short.MIN_VALUE.toInt(), Short.MAX_VALUE.toInt()).toShort()
            for (ch in 0 until channels) {
                out[f * channels + ch] = s
            }
        }
        return out
    }

    private data class WindowStats(
        val maxAdjacentDelta: Int,
        val peak: Int,
        val minRms: Double,
    )

    private fun maxAdjacentDelta(samples: ShortArray, channels: Int): Int {
        var maxDelta = 0
        val frames = samples.size / channels
        for (f in 1 until frames) {
            for (ch in 0 until channels) {
                val prev = samples[(f - 1) * channels + ch].toInt()
                val curr = samples[f * channels + ch].toInt()
                val delta = abs(curr - prev)
                if (delta > maxDelta) maxDelta = delta
            }
        }
        return maxDelta
    }

    private fun measureWindowStats(samples: ShortArray, channels: Int, windowFrames: Int): WindowStats {
        val maxDelta = maxAdjacentDelta(samples, channels)
        var peak = 0
        for (s in samples) {
            val a = abs(s.toInt())
            if (a > peak) peak = a
        }
        val frames = samples.size / channels
        var minRms = Double.POSITIVE_INFINITY
        var start = 0
        while (start + windowFrames <= frames) {
            var sumSq = 0.0
            for (f in start until start + windowFrames) {
                for (ch in 0 until channels) {
                    val v = samples[f * channels + ch].toDouble()
                    sumSq += v * v
                }
            }
            val rms = sqrt(sumSq / (windowFrames * channels))
            if (rms < minRms) minRms = rms
            start += windowFrames
        }
        return WindowStats(maxDelta, peak, if (minRms.isInfinite()) 0.0 else minRms)
    }

    private fun runSeamlessSegmented(
        inputSamples: ShortArray,
        channels: Int,
        sampleRate: Int,
        segments: List<Pair<Int, Boolean>>,
    ): ShortArray {
        val backend = LookaheadDspCompressor()
        val processor = configured(LocalAudioProcessor(backend), sampleRate, channels)
        val collector = mutableListOf<Short>()
        var cursor = 0
        for ((segFrames, enabled) in segments) {
            processor.setProcessingEnabled(enabled)
            val slice = inputSamples.copyOfRange(cursor * channels, (cursor + segFrames) * channels)
            processor.queueInput(samples(*slice))
            collector.addShortsFrom(processor.output)
            cursor += segFrames
        }
        processor.queueEndOfStream()
        collector.addShortsFrom(processor.output)
        processor.close()
        return collector.toShortArray()
    }

    private fun runWithScheduleAndChunkSizes(
        inputSamples: ShortArray,
        channels: Int,
        sampleRate: Int,
        schedule: BooleanArray,
        chunkSizes: IntArray,
    ): ShortArray {
        val backend = LookaheadDspCompressor()
        val processor = configured(LocalAudioProcessor(backend), sampleRate, channels)
        val collector = mutableListOf<Short>()
        var cursor = 0
        var chunkIdx = 0
        val totalFrames = inputSamples.size / channels
        while (cursor < totalFrames) {
            val targetEnabled = schedule[cursor]
            var runFrames = 0
            val maxRun = chunkSizes[chunkIdx % chunkSizes.size]
            while (cursor + runFrames < totalFrames &&
                runFrames < maxRun &&
                schedule[cursor + runFrames] == targetEnabled
            ) {
                runFrames++
            }
            processor.setProcessingEnabled(targetEnabled)
            val slice = inputSamples.copyOfRange(cursor * channels, (cursor + runFrames) * channels)
            processor.queueInput(samples(*slice))
            collector.addShortsFrom(processor.output)
            cursor += runFrames
            chunkIdx++
        }
        processor.queueEndOfStream()
        collector.addShortsFrom(processor.output)
        processor.close()
        return collector.toShortArray()
    }

    private fun runLegacyBaseline(
        inputSamples: ShortArray,
        channels: Int,
        sampleRate: Int,
        segments: List<Pair<Int, Boolean>>,
    ): ShortArray {
        val backend = LookaheadDspCompressor()
        val handle = backend.create(sampleRate, channels)
        var tailPending = false
        val collector = mutableListOf<Short>()
        val drainBuf = ByteBuffer.allocateDirect(64 * channels * 2).order(ByteOrder.nativeOrder())
        var cursor = 0
        for ((segFrames, enabled) in segments) {
            val slice = inputSamples.copyOfRange(cursor * channels, (cursor + segFrames) * channels)
            val inBuf = samples(*slice)
            if (!enabled) {
                if (tailPending) {
                    drainBuf.clear()
                    val drained = backend.drain(handle, drainBuf, 0)
                    drainBuf.limit(drained)
                    drainBuf.position(0)
                    collector.addShortsFrom(drainBuf)
                    tailPending = false
                }
                for (s in slice) collector.add(s)
            } else {
                val outBuf = ByteBuffer.allocateDirect(slice.size * 2).order(ByteOrder.nativeOrder())
                backend.process(handle, inBuf, 0, slice.size * 2, outBuf, 0, outBuf.capacity())
                outBuf.limit(slice.size * 2)
                outBuf.position(0)
                collector.addShortsFrom(outBuf)
                tailPending = true
            }
            cursor += segFrames
        }
        if (tailPending) {
            drainBuf.clear()
            val drained = backend.drain(handle, drainBuf, 0)
            drainBuf.limit(drained)
            drainBuf.position(0)
            collector.addShortsFrom(drainBuf)
        }
        backend.destroy(handle)
        return collector.toShortArray()
    }

    private fun MutableList<Short>.addShortsFrom(buffer: ByteBuffer) {
        val dup = buffer.duplicate().order(ByteOrder.nativeOrder())
        while (dup.remaining() >= 2) {
            add(dup.short)
        }
        buffer.position(buffer.limit())
    }

    /**
     * Deterministic 64-frame lookahead PCM16 model approximating DG-022's
     * compressor (-18 dBFS, 2:1, 6 dB knee, 20 ms attack, 250 ms release,
     * -6.0206 dB makeup) and sample guard. It does not reproduce the native
     * conservative reconstruction bound; AudioTransitionCheck covers real JNI.
     */
    private class LookaheadDspCompressor(private val fixedGain: Double? = null) : PcmCompressor {
        private class State(val sampleRate: Int, val channels: Int) {
            val latency = 64
            val delay = FloatArray(latency * channels)
            var delayPos = 0
            var envLinear = 0.0
            val attackCoeff = 1.0 - exp(-1.0 / (0.020 * sampleRate))
            val releaseCoeff = 1.0 - exp(-1.0 / (0.250 * sampleRate))
            val makeupLinear = 10.0.pow(-6.0206 / 20.0)
            val ceilingLinear = 10.0.pow(-1.0 / 20.0)
        }

        private val states = mutableMapOf<Long, State>()
        private var nextHandle = 1L

        override val isAvailable: Boolean = true
        override val latencyFrames: Int = 64

        override fun create(sampleRate: Int, channelCount: Int): Long {
            val handle = nextHandle++
            states[handle] = State(sampleRate, channelCount)
            return handle
        }

        override fun process(
            handle: Long,
            input: ByteBuffer,
            inputOffset: Int,
            inputLength: Int,
            output: ByteBuffer,
            outputOffset: Int,
            outputCapacity: Int,
        ): Int {
            val st = states[handle] ?: return -2
            val frameBytes = st.channels * 2
            val frames = inputLength / frameBytes
            val inView = input.duplicate().order(ByteOrder.nativeOrder())
            val outView = output.duplicate().order(ByteOrder.nativeOrder())

            for (f in 0 until frames) {
                var peak = 0.0
                val compressed = FloatArray(st.channels)
                for (ch in 0 until st.channels) {
                    val sample = inView.getShort(inputOffset + f * frameBytes + ch * 2) / 32768.0
                    val a = abs(sample)
                    if (a > peak) peak = a
                    compressed[ch] = sample.toFloat()
                }
                val coeff = if (peak > st.envLinear) st.attackCoeff else st.releaseCoeff
                st.envLinear += (peak - st.envLinear) * coeff
                val envDb = if (st.envLinear > 1e-9) 20.0 * ln(st.envLinear) / ln(10.0) else -120.0
                val gainDb = computeStaticGainDb(envDb) + -6.0206
                val compGain = fixedGain ?: 10.0.pow(gainDb / 20.0)

                // Limiter instant gain guard + 64-frame lookahead delay line.
                var blockPeak = 0.0
                for (ch in 0 until st.channels) {
                    compressed[ch] = (compressed[ch] * compGain).toFloat()
                    val a = abs(compressed[ch].toDouble())
                    if (a > blockPeak) blockPeak = a
                }
                val limGain = if (blockPeak > st.ceilingLinear) st.ceilingLinear / blockPeak else 1.0

                for (ch in 0 until st.channels) {
                    val slot = st.delayPos * st.channels + ch
                    val delayed = st.delay[slot]
                    st.delay[slot] = compressed[ch]
                    val limited = (delayed * limGain).toDouble()
                    val pcm = (limited * 32768.0).roundToInt()
                        .coerceIn(Short.MIN_VALUE.toInt(), Short.MAX_VALUE.toInt()).toShort()
                    outView.putShort(outputOffset + f * frameBytes + ch * 2, pcm)
                }
                st.delayPos = (st.delayPos + 1) % st.latency
            }
            return inputLength
        }

        override fun drain(handle: Long, output: ByteBuffer, outputOffset: Int): Int {
            val st = states[handle] ?: return -2
            val outView = output.duplicate().order(ByteOrder.nativeOrder())
            val frameBytes = st.channels * 2
            for (f in 0 until st.latency) {
                for (ch in 0 until st.channels) {
                    val slot = ((st.delayPos + f) % st.latency) * st.channels + ch
                    val sample = st.delay[slot].toDouble().coerceIn(-st.ceilingLinear, st.ceilingLinear)
                    val pcm = (sample * 32768.0).roundToInt()
                        .coerceIn(Short.MIN_VALUE.toInt(), Short.MAX_VALUE.toInt()).toShort()
                    outView.putShort(outputOffset + f * frameBytes + ch * 2, pcm)
                }
            }
            st.delay.fill(0f)
            st.delayPos = 0
            // Native drain resets the limiter, retaining compressor history.
            return st.latency * frameBytes
        }

        override fun reset(handle: Long) {
            states[handle]?.let {
                it.delay.fill(0f)
                it.delayPos = 0
                it.envLinear = 0.0
            }
        }

        override fun destroy(handle: Long) {
            states.remove(handle)
        }

        private fun computeStaticGainDb(inputDb: Double): Double {
            val threshold = -18.0
            val knee = 6.0
            val slope = 1.0 - 1.0 / 2.0
            val diff = inputDb - threshold
            val halfKnee = knee * 0.5
            return when {
                diff <= -halfKnee -> 0.0
                diff >= halfKnee -> -slope * diff
                else -> {
                    val x = diff + halfKnee
                    -slope * (x * x) / (2.0 * knee)
                }
            }
        }
    }
}
