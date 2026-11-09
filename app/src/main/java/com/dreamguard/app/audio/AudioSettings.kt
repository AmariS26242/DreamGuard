package com.dreamguard.app.audio

/**
 * User-facing tone controls. Values are sanitized at the controller boundary so
 * the native renderer never receives NaN, infinity, or out-of-range parameters.
 */
data class AudioSettings(
    val gain: Float = DEFAULT_GAIN,
    val toneCutoffHz: Float = DEFAULT_TONE_CUTOFF_HZ,
    val noiseMix: Float = DEFAULT_NOISE_MIX,
    val droneHz: Float = DEFAULT_DRONE_HZ,
) {
    fun sanitized(): AudioSettings = AudioSettings(
        gain = clampFinite(gain, DEFAULT_GAIN, MIN_GAIN, MAX_GAIN),
        toneCutoffHz = clampFinite(
            toneCutoffHz,
            DEFAULT_TONE_CUTOFF_HZ,
            MIN_TONE_CUTOFF_HZ,
            MAX_TONE_CUTOFF_HZ,
        ),
        noiseMix = clampFinite(noiseMix, DEFAULT_NOISE_MIX, MIN_NOISE_MIX, MAX_NOISE_MIX),
        droneHz = clampFinite(droneHz, DEFAULT_DRONE_HZ, MIN_DRONE_HZ, MAX_DRONE_HZ),
    )

    companion object {
        const val MIN_GAIN = 0f
        const val MAX_GAIN = 1f
        const val DEFAULT_GAIN = 0.7f

        const val MIN_TONE_CUTOFF_HZ = 80f
        const val MAX_TONE_CUTOFF_HZ = 12_000f
        const val DEFAULT_TONE_CUTOFF_HZ = 1_200f

        const val MIN_NOISE_MIX = 0f
        const val MAX_NOISE_MIX = 1f
        const val DEFAULT_NOISE_MIX = 0.35f

        const val MIN_DRONE_HZ = 40f
        const val MAX_DRONE_HZ = 220f
        const val DEFAULT_DRONE_HZ = 110f

        private fun clampFinite(value: Float, fallback: Float, min: Float, max: Float): Float {
            if (!value.isFinite()) return fallback
            return value.coerceIn(min, max)
        }
    }
}
