package com.dreamguard.app.audio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The layer selection is user intent: it has to stay stable across pauses and service
 * restarts, and the two layers must be independently selectable so the service never
 * has to guess from playback state what the user last asked for.
 */
class PlaybackLayersTest {

    @Test
    fun `nothing is selected by default`() {
        val layers = PlaybackLayers()

        assertTrue(layers.isEmpty)
        assertFalse(layers.dsp)
        assertFalse(layers.ambient)
        assertEquals(0f, layers.ambientVolume, 0.0001f)
    }

    @Test
    fun `selecting a layer changes only that layer`() {
        val layers = PlaybackLayers().withDsp()

        assertTrue(layers.dsp)
        assertFalse(layers.ambient)
        assertFalse(layers.isEmpty)
        assertEquals(0f, layers.ambientVolume, 0.0001f)
    }

    @Test
    fun `both layers can be selected and removed independently`() {
        val both = PlaybackLayers().withDsp().withAmbient().withoutDsp()

        assertFalse(both.dsp)
        assertTrue(both.ambient)
        assertEquals(1f, both.ambientVolume, 0.0001f)
        assertTrue(both.withoutAmbient().isEmpty)
    }

    @Test
    fun `ambient volume follows the ambient selection only`() {
        assertEquals(1f, PlaybackLayers(ambient = true).ambientVolume, 0.0001f)
        assertEquals(0f, PlaybackLayers(dsp = true).ambientVolume, 0.0001f)
    }

    @Test
    fun `updates return a new value`() {
        val original = PlaybackLayers(dsp = true)
        val updated = original.withAmbient()

        assertFalse(original.ambient)
        assertTrue(updated.ambient)
        assertTrue(updated.dsp)
    }
}
