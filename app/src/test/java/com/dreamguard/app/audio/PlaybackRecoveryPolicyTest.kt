package com.dreamguard.app.audio

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The start/retry matrix that keeps a deferred (not failed) start from either turning
 * into an unintended autoplay or spinning forever.
 */
class PlaybackRecoveryPolicyTest {

    @Test
    fun `dsp starts only when the user wants playback and it is not running yet`() {
        assertTrue(
            PlaybackRecoveryPolicy.shouldAttemptStart(
                layers = PlaybackLayers(dsp = true),
                wantsPlayback = true,
                alreadyRunning = false,
            ),
        )
    }

    @Test
    fun `an ambient-only session never starts the dsp`() {
        assertFalse(
            PlaybackRecoveryPolicy.shouldAttemptStart(
                layers = PlaybackLayers(ambient = true),
                wantsPlayback = true,
                alreadyRunning = false,
            ),
        )
    }

    @Test
    fun `a paused player does not start the dsp`() {
        assertFalse(
            PlaybackRecoveryPolicy.shouldAttemptStart(
                layers = PlaybackLayers(dsp = true),
                wantsPlayback = false,
                alreadyRunning = false,
            ),
        )
    }

    @Test
    fun `an already running engine is not started twice`() {
        assertFalse(
            PlaybackRecoveryPolicy.shouldAttemptStart(
                layers = PlaybackLayers(dsp = true),
                wantsPlayback = true,
                alreadyRunning = true,
            ),
        )
    }

    @Test
    fun `a deferred start is retried while the user still wants playback`() {
        assertTrue(
            PlaybackRecoveryPolicy.shouldRetry(
                retryableStart = true,
                layers = PlaybackLayers(dsp = true),
                wantsPlayback = true,
                retriesSoFar = 0,
            ),
        )
        assertTrue(
            PlaybackRecoveryPolicy.shouldRetry(
                retryableStart = true,
                layers = PlaybackLayers(dsp = true, ambient = true),
                wantsPlayback = true,
                retriesSoFar = PlaybackRecoveryPolicy.MAX_START_RETRIES - 1,
            ),
        )
    }

    @Test
    fun `the retry budget is bounded`() {
        assertFalse(
            PlaybackRecoveryPolicy.shouldRetry(
                retryableStart = true,
                layers = PlaybackLayers(dsp = true),
                wantsPlayback = true,
                retriesSoFar = PlaybackRecoveryPolicy.MAX_START_RETRIES,
            ),
        )
    }

    @Test
    fun `a real failure is not retried as a deferred start`() {
        assertFalse(
            PlaybackRecoveryPolicy.shouldRetry(
                retryableStart = false,
                layers = PlaybackLayers(dsp = true),
                wantsPlayback = true,
                retriesSoFar = 0,
            ),
        )
    }

    @Test
    fun `the user can cancel playback while a start is deferred`() {
        assertFalse(
            PlaybackRecoveryPolicy.shouldRetry(
                retryableStart = true,
                layers = PlaybackLayers(dsp = true),
                wantsPlayback = false,
                retriesSoFar = 0,
            ),
        )
        assertFalse(
            PlaybackRecoveryPolicy.shouldRetry(
                retryableStart = true,
                layers = PlaybackLayers(ambient = true),
                wantsPlayback = true,
                retriesSoFar = 0,
            ),
        )
    }
}
