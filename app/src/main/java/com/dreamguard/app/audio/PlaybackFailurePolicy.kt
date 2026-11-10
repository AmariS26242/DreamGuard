package com.dreamguard.app.audio

/**
 * Reactions to asynchronous DSP failures, kept free of Android types so the
 * decision matrix is unit-testable.
 *
 * The DSP layer is silent by definition while it is broken. If it is the only
 * selected layer, leaving the Media3 player running would keep a foreground
 * media session and its notification alive while producing no audio, so the
 * player has to be paused. With the ambient loop selected the player still has
 * something audible to do: it keeps running, and only the error is surfaced.
 */
internal object PlaybackFailurePolicy {

    /** Whether a failed DSP render thread requires pausing the Media3 player. */
    fun shouldPausePlayer(layers: PlaybackLayers): Boolean = layers.dsp && !layers.ambient

    /**
     * Whether a `playWhenReady = false` event should end the service. Only an explicit
     * user pause does: a pause issued to clean up an internal failure has to keep the
     * session (and its error) alive.
     */
    fun shouldStopServiceOnPause(
        isUserRequest: Boolean,
        isInternalCleanup: Boolean,
    ): Boolean = isUserRequest && !isInternalCleanup
}
