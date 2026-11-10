package com.dreamguard.app.audio

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PlaybackFailurePolicyTest {

    @Test
    fun `dsp-only failure pauses the silent player`() {
        assertTrue(PlaybackFailurePolicy.shouldPausePlayer(PlaybackLayers(dsp = true)))
    }

    @Test
    fun `ambient layer keeps the player running when the dsp fails`() {
        assertFalse(
            PlaybackFailurePolicy.shouldPausePlayer(PlaybackLayers(dsp = true, ambient = true)),
        )
    }

    @Test
    fun `ambient-only session is never touched by a dsp failure`() {
        assertFalse(PlaybackFailurePolicy.shouldPausePlayer(PlaybackLayers(ambient = true)))
    }

    @Test
    fun `session without layers has nothing to pause`() {
        assertFalse(PlaybackFailurePolicy.shouldPausePlayer(PlaybackLayers()))
    }

    @Test
    fun `an explicit user pause ends the service`() {
        assertTrue(
            PlaybackFailurePolicy.shouldStopServiceOnPause(
                isUserRequest = true,
                isInternalCleanup = false,
            ),
        )
    }

    @Test
    fun `a pause for an internal failure keeps the service and its error alive`() {
        assertFalse(
            PlaybackFailurePolicy.shouldStopServiceOnPause(
                isUserRequest = true,
                isInternalCleanup = true,
            ),
        )
    }

    @Test
    fun `a pause from another reason is not treated as a user pause`() {
        assertFalse(
            PlaybackFailurePolicy.shouldStopServiceOnPause(
                isUserRequest = false,
                isInternalCleanup = false,
            ),
        )
    }

    @Test
    fun `a non-user pause is never a stop even without cleanup`() {
        assertFalse(
            PlaybackFailurePolicy.shouldStopServiceOnPause(
                isUserRequest = false,
                isInternalCleanup = true,
            ),
        )
    }
}
