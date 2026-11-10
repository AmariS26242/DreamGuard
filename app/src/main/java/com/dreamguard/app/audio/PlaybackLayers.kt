package com.dreamguard.app.audio

/**
 * Which sound layers the user has selected.
 *
 * This is user intent, not playback state: pausing, losing audio focus, or a service
 * recreation must not change it, and [PlaybackService] persists it. Keeping both
 * layers in one immutable value removes the ambiguity where one flag described the
 * session and the other described production.
 */
data class PlaybackLayers(
    val dsp: Boolean = false,
    val ambient: Boolean = false,
) {
    val isEmpty: Boolean get() = !dsp && !ambient

    /**
     * Media3 track volume. The track stays silent while only the DSP layer is selected
     * so both layers share one player, one session, and one audio-focus decision.
     */
    val ambientVolume: Float get() = if (ambient) 1f else 0f

    fun withDsp(): PlaybackLayers = copy(dsp = true)

    fun withoutDsp(): PlaybackLayers = copy(dsp = false)

    fun withAmbient(): PlaybackLayers = copy(ambient = true)

    fun withoutAmbient(): PlaybackLayers = copy(ambient = false)
}
