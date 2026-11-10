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

        override fun destroy(handle: Long) {
            destroyed += handle
        }
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
        val second = samples(4000, -4000)
        processor.queueInput(second)
        assertArrayEquals(samples(2000, -2000).toByteArray(), processor.output.toByteArray())
        assertEquals(1, fake.processCalls)

        processor.setProcessingEnabled(false)
        val third = samples(-3, 3)
        processor.queueInput(third)
        // Truncation toward zero is only visible while enabled; bypass restores bytes.
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
}
