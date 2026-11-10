package com.dreamguard.app.audio

import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/**
 * Exercises the threading and teardown contract of [NativeAudioEngine] on a plain JVM:
 * the native DSP and the float output are replaced with fakes, so the tests describe
 * exactly what the render thread is doing (blocked in write, failing, exiting) and
 * when the native handle may be destroyed.
 */
class NativeAudioEngineTest {

    @Test
    fun `stop stays bounded while the render thread is blocked in write`() {
        val dsp = FakeDsp()
        val outputs = FakeOutputs(blocking = true)
        val engine = NativeAudioEngine(dsp, outputs)
        try {
            assertTrue(engine.start())
            val output = outputs.opened.single()
            assertTrue("render thread never reached write()", output.enteredWrite.await(2, TimeUnit.SECONDS))

            val startedAt = System.nanoTime()
            engine.stop()
            val elapsedMs = (System.nanoTime() - startedAt) / 1_000_000

            assertTrue("stop() blocked for $elapsedMs ms", elapsedMs < 1_500)
            assertEquals(EngineState.STOPPED, engine.state)
            assertTrue("native handle freed while the render thread still uses it", dsp.destroyed.isEmpty())
            assertFalse("AudioTrack released before the render thread returned", output.released.get())
        } finally {
            outputs.opened.forEach { it.gate.countDown() }
            awaitUntil("render thread finishes") { dsp.resets.isNotEmpty() || dsp.destroyed.isNotEmpty() }
            engine.release()
        }
        assertEquals(listOf(1_000L), dsp.destroyed)
    }

    @Test
    fun `release defers the native destroy until a wedged render thread exits`() {
        val dsp = FakeDsp()
        val outputs = FakeOutputs(blocking = true)
        val engine = NativeAudioEngine(dsp, outputs)
        assertTrue(engine.start())
        val output = outputs.opened.single()
        assertTrue(output.enteredWrite.await(2, TimeUnit.SECONDS))

        engine.release()

        assertTrue("handle destroyed while the render thread still renders", dsp.destroyed.isEmpty())
        assertNotEquals(EngineState.RELEASED, engine.state)

        output.gate.countDown()
        awaitUntil("postponed destroy runs on thread exit") { engine.state == EngineState.RELEASED }

        assertEquals(listOf(1_000L), dsp.destroyed)
        assertTrue("output was not released by the render thread", output.released.get())
    }

    @Test
    fun `render thread failure surfaces as ERROR with a message and a callback`() {
        val dsp = FakeDsp()
        val outputs = FakeOutputs(blocking = false, failCode = -3)
        val engine = NativeAudioEngine(dsp, outputs)
        val callbacks = AtomicInteger()
        engine.onStateChanged = { callbacks.incrementAndGet() }
        try {
            assertTrue(engine.start())
            awaitUntil("render failure reported") { engine.state == EngineState.ERROR }

            val failure = engine.lastFailure
            assertNotNull(failure)
            assertTrue("unexpected message: $failure", failure!!.contains("Audio output rejected"))
            assertEquals(
                "a rejected write is a transient output failure",
                EngineFailureKind.TRANSIENT,
                engine.failureKind,
            )
            assertTrue("no state callback for the failure", callbacks.get() > 0)
        } finally {
            engine.release()
        }
    }

    @Test
    fun `renderer returning no frames is reported as a failure`() {
        val dsp = FakeDsp().apply { renderResult = 0 }
        val outputs = FakeOutputs(blocking = false)
        val engine = NativeAudioEngine(dsp, outputs)
        try {
            assertTrue(engine.start())
            awaitUntil("renderer failure reported") { engine.state == EngineState.ERROR }
            assertTrue(engine.lastFailure!!.contains("Native renderer returned 0"))
            assertEquals(
                "a broken native renderer is not retryable",
                EngineFailureKind.PERMANENT,
                engine.failureKind,
            )
        } finally {
            engine.release()
        }
    }

    @Test
    fun `controller observes an asynchronous engine failure`() {
        val dsp = FakeDsp()
        val outputs = FakeOutputs(blocking = false, failCode = -5)
        val controller = PlaybackController(NativeAudioEngine(dsp, outputs))
        assertTrue(controller.start())

        awaitUntil("controller mirrors the engine failure") { !controller.isRunning }
        assertTrue(controller.lastError!!.contains("Audio output rejected"))
        assertFalse(controller.isRunning)

        controller.release()
    }

    @Test
    fun `start is rejected without leaving ERROR while the previous thread exits`() {
        val dsp = FakeDsp()
        val outputs = FakeOutputs(blocking = true)
        val engine = NativeAudioEngine(dsp, outputs)
        assertTrue(engine.start())
        val output = outputs.opened.single()
        assertTrue(output.enteredWrite.await(2, TimeUnit.SECONDS))
        engine.stop()
        assertEquals(EngineState.STOPPED, engine.state)

        assertFalse("two render threads shared one handle", engine.start())
        assertEquals(EngineState.STOPPED, engine.state)
        assertTrue(engine.lastFailure!!.contains("still shutting down"))
        assertEquals(
            "deferring a start is not an engine failure",
            EngineFailureKind.NONE,
            engine.failureKind,
        )
        assertEquals("a deferred start opened a second output", 1, outputs.opened.size)

        output.gate.countDown()
        awaitUntil("previous thread finishes") { dsp.resets.isNotEmpty() }
        engine.release()
        assertEquals(listOf(1_000L), dsp.destroyed)
    }

    @Test
    fun `stop before start and release are idempotent`() {
        val dsp = FakeDsp()
        val engine = NativeAudioEngine(dsp, FakeOutputs(blocking = false))

        engine.stop()
        assertEquals(EngineState.IDLE, engine.state)

        engine.release()
        assertEquals(EngineState.RELEASED, engine.state)
        assertFalse("start() accepted after release", engine.start())
        assertEquals(EngineState.RELEASED, engine.state)
        assertEquals(0, dsp.destroyed.size)
    }

    @Test
    fun `missing native library fails the start instead of throwing`() {
        val dsp = FakeDsp().apply { isLoaded = false }
        val engine = NativeAudioEngine(dsp, FakeOutputs(blocking = false))

        assertFalse(engine.start())
        assertEquals(EngineState.ERROR, engine.state)
        assertFalse(engine.isAvailable)
        assertEquals(EngineFailureKind.PERMANENT, engine.failureKind)
        engine.release()
    }

    @Test
    fun `stop never touches the audio output from the caller thread`() {
        val dsp = FakeDsp()
        val outputs = FakeOutputs(blocking = true)
        val engine = NativeAudioEngine(dsp, outputs)
        val caller = Thread.currentThread().name
        try {
            assertTrue(engine.start())
            val output = outputs.opened.single()
            assertTrue(output.enteredWrite.await(2, TimeUnit.SECONDS))

            engine.stop()

            awaitUntil("wake-up thread services the output") { output.pausedFrom.isNotEmpty() }
            val pausedFrom = output.pausedFrom.first()
            assertNotEquals(
                "stop() performed the AudioTrack control call on the caller thread",
                caller,
                pausedFrom,
            )
            assertTrue(
                "expected the dedicated wake-up thread, got $pausedFrom",
                pausedFrom.startsWith("dreamguard-stop"),
            )
        } finally {
            outputs.opened.forEach { it.gate.countDown() }
            awaitUntil("render thread finishes") { dsp.resets.isNotEmpty() || dsp.destroyed.isNotEmpty() }
            engine.release()
        }
    }

    @Test
    fun `a failed session can be restarted and clears the failure`() {
        val dsp = FakeDsp()
        val outputs = FakeOutputs(blocking = false, failCode = -3)
        val engine = NativeAudioEngine(dsp, outputs)
        try {
            assertTrue(engine.start())
            awaitUntil("render failure reported") { engine.state == EngineState.ERROR }
            assertNotNull(engine.lastFailure)
            assertTrue("failed session leaked its DSP state", dsp.resets.contains(1_000L))

            outputs.failCode = Int.MIN_VALUE
            assertTrue("restart after a render failure was rejected", engine.start())

            assertEquals(EngineState.RUNNING, engine.state)
            assertNull("failure message survived a successful restart", engine.lastFailure)
        } finally {
            engine.release()
        }
    }

    @Test
    fun `a write that never accepts frames fails after a bounded number of attempts`() {
        val dsp = FakeDsp()
        val outputs = FakeOutputs(blocking = false, failCode = 0)
        val engine = NativeAudioEngine(dsp, outputs)
        try {
            assertTrue(engine.start())
            awaitUntil("zero-progress failure reported", timeoutMs = 5_000) {
                engine.state == EngineState.ERROR
            }

            val writes = outputs.opened.single().writeCount.get()
            assertTrue("the renderer gave up without retrying (writes=$writes)", writes >= 20)
            assertTrue("the render thread kept spinning on zero writes (writes=$writes)", writes < 200)
            assertTrue(engine.lastFailure!!.contains("no progress"))
            assertEquals(EngineFailureKind.TRANSIENT, engine.failureKind)
        } finally {
            engine.release()
        }
    }

    @Test
    fun `repeated stop while the HAL blocks creates only one wake-up thread`() {
        val dsp = FakeDsp()
        val outputs = FakeOutputs(blocking = true)
        val engine = NativeAudioEngine(dsp, outputs)
        val pauseGate = CountDownLatch(1)
        val baseline = stopThreadCount()
        try {
            assertTrue(engine.start())
            val output = outputs.opened.single()
            output.blockPause = pauseGate
            assertTrue(output.enteredWrite.await(2, TimeUnit.SECONDS))

            val startedAt = System.nanoTime()
            repeat(3) { engine.stop() }
            val elapsedMs = (System.nanoTime() - startedAt) / 1_000_000

            assertTrue("three stop() calls blocked for $elapsedMs ms", elapsedMs < 2_000)
            assertEquals(
                "repeated stop spawned one wake-up thread per call",
                baseline + 1,
                stopThreadCount(),
            )
            assertEquals("more than one wake-up thread entered pause()", 1, output.pausedFrom.size)
            assertFalse(output.released.get())
            assertTrue("handle freed while the render thread was stuck", dsp.destroyed.isEmpty())
        } finally {
            pauseGate.countDown()
            outputs.opened.forEach { it.gate.countDown() }
            awaitUntil("render thread finishes") { dsp.resets.isNotEmpty() || dsp.destroyed.isNotEmpty() }
            engine.release()
        }
    }

    @Test
    fun `repeated start while the old render thread exits reuses one session`() {
        val dsp = FakeDsp()
        val outputs = FakeOutputs(blocking = true)
        val engine = NativeAudioEngine(dsp, outputs)
        try {
            assertTrue(engine.start())
            val output = outputs.opened.single()
            assertTrue(output.enteredWrite.await(2, TimeUnit.SECONDS))
            engine.stop()

            repeat(5) {
                assertFalse("start() created a second render session", engine.start())
                assertEquals(EngineState.STOPPED, engine.state)
                assertEquals(EngineFailureKind.NONE, engine.failureKind)
            }
            assertEquals("each rejected start created a native handle", 1, dsp.created.size)
            assertEquals("each rejected start opened another output", 1, outputs.opened.size)

            output.gate.countDown()
            awaitUntil("previous session finishes") { dsp.resets.isNotEmpty() }
            engine.release()
            assertEquals(listOf(1_000L), dsp.destroyed)
        } finally {
            outputs.opened.forEach { it.gate.countDown() }
            awaitUntil("render thread finishes") { dsp.resets.isNotEmpty() || dsp.destroyed.isNotEmpty() }
            engine.release()
        }
    }

    @Test
    fun `an output that cannot open is transient and reuses the DSP handle`() {
        val dsp = FakeDsp()
        val outputs = FakeOutputs(blocking = false).apply { openFails = true }
        val engine = NativeAudioEngine(dsp, outputs)
        try {
            assertFalse(engine.start())
            assertEquals(EngineState.ERROR, engine.state)
            assertEquals(EngineFailureKind.TRANSIENT, engine.failureKind)
            assertTrue(engine.lastFailure!!.contains("could not be opened"))
            assertEquals(listOf(1_000L), dsp.created)
            assertTrue(dsp.destroyed.isEmpty())

            outputs.openFails = false
            assertTrue("a transient output error was not retryable", engine.start())
            assertEquals(EngineState.RUNNING, engine.state)
            assertEquals("the retry created another native handle", listOf(1_000L), dsp.created)
            assertNull(engine.lastFailure)
        } finally {
            engine.release()
        }
        assertEquals(listOf(1_000L), dsp.destroyed)
    }

    private fun stopThreadCount(): Int =
        Thread.getAllStackTraces().keys.count { it.name.startsWith("dreamguard-stop") }

    private fun awaitUntil(what: String, timeoutMs: Long = 3_000, condition: () -> Boolean) {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            if (condition()) return
            Thread.sleep(5)
        }
        fail("timed out waiting for $what")
    }

    private class FakeDsp : DspCommands {
        override var isLoaded: Boolean = true

        @Volatile
        var renderResult: Int = 1024

        val created = CopyOnWriteArrayList<Long>()
        val destroyed = CopyOnWriteArrayList<Long>()
        val resets = CopyOnWriteArrayList<Long>()
        private var nextHandle = 1_000L

        override fun create(sampleRate: Int, channels: Int): Long {
            val handle = nextHandle++
            created += handle
            return handle
        }

        override fun destroy(handle: Long) {
            destroyed += handle
        }

        override fun reset(handle: Long) {
            resets += handle
        }

        override fun render(handle: Long, buffer: FloatArray, frames: Int): Int = renderResult

        override fun setGain(handle: Long, gain: Float) = Unit

        override fun setToneCutoff(handle: Long, cutoffHz: Float) = Unit

        override fun setNoiseMix(handle: Long, mix: Float) = Unit

        override fun setDroneHz(handle: Long, hz: Float) = Unit

        override fun version(): String = "test-0.1.0"
    }

    private class FakeOutputs(
        private val blocking: Boolean,
        @Volatile var failCode: Int = Int.MIN_VALUE,
    ) : RenderOutputFactory {
        val opened = CopyOnWriteArrayList<FakeOutput>()

        @Volatile
        var openFails = false

        override fun prepareRenderThread() = Unit

        override fun open(sampleRate: Int, channels: Int): RenderOutput? {
            if (openFails) return null
            return FakeOutput(blocking, failCode).also { opened += it }
        }
    }

    private class FakeOutput(
        private val blocking: Boolean,
        @Volatile private var failCode: Int,
    ) : RenderOutput {
        val enteredWrite = CountDownLatch(1)
        val gate = CountDownLatch(1)
        val released = AtomicBoolean(false)
        val pausedFrom = CopyOnWriteArrayList<String>()
        val writeCount = AtomicInteger()

        /** Lets a test park the wake-up thread inside a HAL call. */
        @Volatile
        var blockPause: CountDownLatch? = null

        override fun play() = Unit

        override fun write(buffer: FloatArray, offset: Int, size: Int): Int {
            writeCount.incrementAndGet()
            enteredWrite.countDown()
            if (failCode != Int.MIN_VALUE) return failCode
            if (blocking) gate.await(10, TimeUnit.SECONDS)
            return size
        }

        override fun pause() {
            pausedFrom += Thread.currentThread().name
            blockPause?.await(10, TimeUnit.SECONDS)
        }

        override fun flush() = Unit

        override fun stop() = Unit

        override fun release() {
            released.set(true)
        }
    }
}
