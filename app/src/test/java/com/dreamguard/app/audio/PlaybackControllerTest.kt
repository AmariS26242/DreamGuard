package com.dreamguard.app.audio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
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
}
