package com.dreamguard.app.audio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PlaybackControllerTest {

    @Test
    fun `start forwards the sanitized settings and marks running`() {
        val engine = FakeAudioEngine()
        val controller = PlaybackController(
            engine,
            AudioSettings(gain = 0.4f, toneCutoffHz = 2_000f, noiseMix = 0.5f, droneHz = 90f),
        )

        assertTrue(controller.start())
        assertTrue(controller.isRunning)
        assertEquals(0.4f, engine.lastGain!!, 0.0001f)
        assertEquals(2_000f, engine.lastCutoffHz!!, 0.0001f)
        assertEquals(0.5f, engine.lastNoiseMix!!, 0.0001f)
        assertEquals(90f, engine.lastDroneHz!!, 0.0001f)
        assertEquals(listOf("gain=0.4", "cutoff=2000.0", "noise=0.5", "drone=90.0", "start"), engine.calls)
    }

    @Test
    fun `unavailable native library fails with a message and never starts`() {
        val engine = FakeAudioEngine(isAvailable = false)
        val controller = PlaybackController(engine)

        assertFalse(controller.start())
        assertFalse(controller.isRunning)
        assertNotNull(controller.lastError)
        assertTrue(engine.calls.isEmpty())
    }

    @Test
    fun `gain and noise setters clamp input before reaching the engine`() {
        val engine = FakeAudioEngine()
        val controller = PlaybackController(engine)

        controller.setGain(-5f)
        controller.setNoiseMix(3f)

        assertEquals(AudioSettings.MIN_GAIN, controller.settings.gain, 0.0001f)
        assertEquals(AudioSettings.MAX_NOISE_MIX, controller.settings.noiseMix, 0.0001f)
        assertEquals(AudioSettings.MIN_GAIN, engine.lastGain!!, 0.0001f)
        assertEquals(AudioSettings.MAX_NOISE_MIX, engine.lastNoiseMix!!, 0.0001f)
    }

    @Test
    fun `toggle starts then stops and keeps one source of truth`() {
        val engine = FakeAudioEngine()
        val controller = PlaybackController(engine)

        assertTrue(controller.toggle())
        assertTrue(controller.isRunning)

        assertFalse(controller.toggle())
        assertFalse(controller.isRunning)
        assertEquals(EngineState.STOPPED, engine.state)
        assertTrue(engine.calls.contains("stop"))
    }

    @Test
    fun `release is terminal and start afterwards is rejected`() {
        val engine = FakeAudioEngine()
        val controller = PlaybackController(engine)

        controller.release()
        assertFalse(controller.start())
        assertFalse(controller.isRunning)
        assertEquals(EngineState.RELEASED, engine.state)
        assertEquals(1, engine.calls.count { it == "release" })
    }

    @Test
    fun `listener observes state and settings changes`() {
        val engine = FakeAudioEngine()
        val controller = PlaybackController(engine)
        var notifications = 0
        controller.listener = { notifications++ }

        controller.setGain(0.5f)
        controller.start()
        controller.stop()

        assertTrue(notifications >= 3)
    }

    @Test
    fun `failed engine start surfaces an error`() {
        val engine = FakeAudioEngine(startResult = false)
        val controller = PlaybackController(engine)

        assertFalse(controller.start())
        assertFalse(controller.isRunning)
        assertNotNull(controller.lastError)
    }

    @Test
    fun `asynchronous render failure clears running and reports the engine message`() {
        val engine = FakeAudioEngine()
        val controller = PlaybackController(engine)
        var notifications = 0
        controller.listener = { notifications++ }

        assertTrue(controller.start())
        engine.failAsync("Audio output rejected the block (-3)")

        assertFalse(controller.isRunning)
        assertEquals("Audio output rejected the block (-3)", controller.lastError)
        assertEquals(EngineState.ERROR, controller.snapshot().engineState)
        assertTrue(notifications >= 2)
    }

    @Test
    fun `asynchronous stop clears running without inventing an error`() {
        val engine = FakeAudioEngine()
        val controller = PlaybackController(engine)

        assertTrue(controller.start())
        engine.stopAsync()

        assertFalse(controller.isRunning)
        assertNull(controller.lastError)
    }

    @Test
    fun `restart after a render failure clears the error`() {
        val engine = FakeAudioEngine()
        val controller = PlaybackController(engine)

        assertTrue(controller.start())
        engine.failAsync("Audio output rejected the block (-3)")
        assertFalse(controller.isRunning)
        assertNotNull(controller.lastError)

        assertTrue(controller.start())

        assertTrue(controller.isRunning)
        assertNull("a successful restart must not keep the old error", controller.lastError)
    }

    @Test
    fun `each engine failure is counted once`() {
        val engine = FakeAudioEngine()
        val controller = PlaybackController(engine)
        assertEquals(0, controller.engineFailureCount)

        assertTrue(controller.start())
        engine.failAsync("first failure")
        assertEquals(1, controller.engineFailureCount)

        assertTrue(controller.start())
        engine.failAsync("second failure")
        assertEquals(2, controller.engineFailureCount)

        controller.stop()
        assertEquals("a normal stop is not a failure", 2, controller.engineFailureCount)
    }

    @Test
    fun `release detaches the engine callback so late failures are ignored`() {
        val engine = FakeAudioEngine()
        val controller = PlaybackController(engine)

        controller.release()
        engine.failAsync("late failure")

        assertFalse(controller.isRunning)
        assertNull(controller.lastError)
    }

    @Test
    fun `a deferred start is retryable and is not reported as a failure`() {
        val engine = FakeAudioEngine(startDeferred = true)
        val controller = PlaybackController(engine)

        assertFalse(controller.start())

        assertFalse(controller.isRunning)
        assertTrue("the service cannot tell it should retry", controller.retryableStart)
        assertNull("a deferred start must not look like an error", controller.lastError)
        assertEquals(EngineFailureKind.NONE, controller.failureKind)
        assertEquals("a deferred start is not an engine failure", 0, controller.engineFailureCount)
        assertTrue(controller.snapshot().retryableStart)
        assertEquals(EngineFailureKind.NONE, controller.snapshot().failureKind)
    }

    @Test
    fun `abandoning a deferred start clears it and notifies once`() {
        val engine = FakeAudioEngine(startDeferred = true)
        val controller = PlaybackController(engine)
        var notifications = 0
        controller.listener = { notifications++ }
        assertFalse(controller.start())
        val beforeAbandon = notifications

        controller.abandonDeferredStart()

        assertFalse(controller.retryableStart)
        assertTrue("the UI was not told the retry window closed", notifications > beforeAbandon)

        val afterAbandon = notifications
        controller.abandonDeferredStart()
        assertEquals("nothing left to abandon", afterAbandon, notifications)
    }

    @Test
    fun `a successful start clears a previous deferred start`() {
        val engine = FakeAudioEngine(startDeferred = true)
        val controller = PlaybackController(engine)
        assertFalse(controller.start())
        assertTrue(controller.retryableStart)

        engine.startDeferred = false
        assertTrue(controller.start())

        assertTrue(controller.isRunning)
        assertFalse(controller.retryableStart)
        assertNull(controller.lastError)
        assertEquals(EngineFailureKind.NONE, controller.failureKind)
    }

    @Test
    fun `stop clears a deferred start`() {
        val engine = FakeAudioEngine(startDeferred = true)
        val controller = PlaybackController(engine)
        assertFalse(controller.start())
        assertTrue(controller.retryableStart)

        controller.stop()

        assertFalse(controller.retryableStart)
        assertFalse(controller.isRunning)
        assertEquals(EngineState.STOPPED, controller.snapshot().engineState)
    }

    @Test
    fun `the failure kind is copied from the engine`() {
        val engine = FakeAudioEngine()
        val controller = PlaybackController(engine)
        assertTrue(controller.start())

        engine.failAsync("Audio output could not be opened.", EngineFailureKind.TRANSIENT)

        assertEquals(EngineFailureKind.TRANSIENT, controller.failureKind)
        assertEquals(EngineFailureKind.TRANSIENT, controller.snapshot().failureKind)

        // A permanent failure must stay distinguishable so the UI/service never
        // pretends that re-selecting the same output can work.
        assertTrue(controller.start())
        engine.failAsync("Native renderer returned 0 frames.", EngineFailureKind.PERMANENT)
        assertEquals(EngineFailureKind.PERMANENT, controller.snapshot().failureKind)
    }
}
