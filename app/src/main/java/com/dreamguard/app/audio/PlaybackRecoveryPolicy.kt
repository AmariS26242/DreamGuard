package com.dreamguard.app.audio

/**
 * Bounded recovery rules for a DSP start that lost a race with the previous render
 * thread's shutdown. Kept free of Android types so the decision matrix is JVM-testable
 * and stays separate from the Media3 listener code that drives it.
 *
 * The retry is deliberately tiny. A render thread that has not returned within
 * [MAX_START_RETRIES] × [RETRY_DELAY_MS] is most likely wedged in the HAL; at that
 * point the honest response is to surface a retryable error and stop trying, not to
 * keep spawning threads or to silently disable the user's selected layer.
 */
internal object PlaybackRecoveryPolicy {

    /** Maximum number of automatic follow-up starts after a deferred start. */
    const val MAX_START_RETRIES = 6

    /** Delay between those attempts; about 1.5 s of total grace. */
    const val RETRY_DELAY_MS = 250L

    /**
     * Whether playback should (re)start the DSP layer now. [wantsPlayback] is the
     * player's `playWhenReady`, not `isPlaying`, so a transient buffering state never
     * turns into an unintended autoplay.
     */
    fun shouldAttemptStart(
        layers: PlaybackLayers,
        wantsPlayback: Boolean,
        alreadyRunning: Boolean,
    ): Boolean = layers.dsp && wantsPlayback && !alreadyRunning

    /**
     * Whether a start that came back deferred (old render thread still alive) deserves
     * another automatic attempt. [retriesSoFar] counts attempts already made.
     */
    fun shouldRetry(
        retryableStart: Boolean,
        layers: PlaybackLayers,
        wantsPlayback: Boolean,
        retriesSoFar: Int,
    ): Boolean = retryableStart &&
        layers.dsp &&
        wantsPlayback &&
        retriesSoFar < MAX_START_RETRIES
}
