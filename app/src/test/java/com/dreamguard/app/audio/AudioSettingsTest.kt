package com.dreamguard.app.audio

import org.junit.Assert.assertEquals
import org.junit.Test

class AudioSettingsTest {

    @Test
    fun `defaults are already within range`() {
        val settings = AudioSettings().sanitized()

        assertEquals(AudioSettings.DEFAULT_GAIN, settings.gain, 0.0001f)
        assertEquals(AudioSettings.DEFAULT_TONE_CUTOFF_HZ, settings.toneCutoffHz, 0.0001f)
        assertEquals(AudioSettings.DEFAULT_NOISE_MIX, settings.noiseMix, 0.0001f)
        assertEquals(AudioSettings.DEFAULT_DRONE_HZ, settings.droneHz, 0.0001f)
    }

    @Test
    fun `out of range values are clamped`() {
        val settings = AudioSettings(
            gain = 4f,
            toneCutoffHz = 1f,
            noiseMix = -2f,
            droneHz = 999f,
        ).sanitized()

        assertEquals(AudioSettings.MAX_GAIN, settings.gain, 0.0001f)
        assertEquals(AudioSettings.MIN_TONE_CUTOFF_HZ, settings.toneCutoffHz, 0.0001f)
        assertEquals(AudioSettings.MIN_NOISE_MIX, settings.noiseMix, 0.0001f)
        assertEquals(AudioSettings.MAX_DRONE_HZ, settings.droneHz, 0.0001f)
    }

    @Test
    fun `non finite values fall back to defaults`() {
        val settings = AudioSettings(
            gain = Float.NaN,
            toneCutoffHz = Float.POSITIVE_INFINITY,
            noiseMix = Float.NEGATIVE_INFINITY,
            droneHz = Float.NaN,
        ).sanitized()

        assertEquals(AudioSettings.DEFAULT_GAIN, settings.gain, 0.0001f)
        assertEquals(AudioSettings.DEFAULT_TONE_CUTOFF_HZ, settings.toneCutoffHz, 0.0001f)
        assertEquals(AudioSettings.DEFAULT_NOISE_MIX, settings.noiseMix, 0.0001f)
        assertEquals(AudioSettings.DEFAULT_DRONE_HZ, settings.droneHz, 0.0001f)
    }
}
