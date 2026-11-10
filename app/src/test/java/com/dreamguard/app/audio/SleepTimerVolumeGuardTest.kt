package com.dreamguard.app.audio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * DG-025 regression tests for the guarded timer write path.
 *
 * The timer must not write a stale target over a value that changed after its
 * tick snapshot, must never raise the channel, and the in-app slider must be able
 * to invalidate a queued timer write through the shared barrier. Ordinary DG-024
 * slider writes keep the original setIndex semantics.
 */
class SleepTimerVolumeGuardTest {

    private class FakeBackend(
        var min: Int = 0,
        var max: Int = 15,
        var current: Int = 8,
    ) : MediaVolumeBackend {
        val writes = mutableListOf<Int>()
        var applyWrite: (Int) -> Unit = { current = it }

        override fun isVolumeFixed(): Boolean = false

        override fun minIndex(): Int = min

        override fun maxIndex(): Int = max

        override fun currentIndex(): Int = current

        override fun setIndex(index: Int) {
            writes += index
            applyWrite(index)
        }
    }

    private class MutableClock(var nowMs: Long = 1_000L) : SleepTimerClock {
        override fun elapsedRealtimeMs(): Long = nowMs
    }

    @Test
    fun `guarded write refuses when the channel changed after the timer read`() {
        val backend = FakeBackend(current = 5)
        val controller = SystemVolumeController(backend)

        val result = controller.setIndexGuarded(requestedIndex = 7, expectedIndex = 8)

        assertEquals(SystemVolumeWriteResult.Outcome.EXTERNAL, result.outcome)
        assertEquals(5, result.appliedIndex)
        assertFalse(result.isApplied)
        assertTrue(backend.writes.isEmpty())
    }

    @Test
    fun `guarded write never raises or keeps the same level`() {
        val backend = FakeBackend(current = 5)
        val controller = SystemVolumeController(backend)

        assertEquals(
            SystemVolumeWriteResult.Outcome.NOT_LOWER,
            controller.setIndexGuarded(requestedIndex = 5, expectedIndex = 5).outcome,
        )
        assertEquals(
            SystemVolumeWriteResult.Outcome.NOT_LOWER,
            controller.setIndexGuarded(requestedIndex = 6, expectedIndex = 5).outcome,
        )

        assertTrue(backend.writes.isEmpty())
        assertEquals(5, backend.current)
    }

    @Test
    fun `guarded write clamps below the platform minimum and still only lowers`() {
        val backend = FakeBackend(min = 2, current = 5)
        val controller = SystemVolumeController(backend)

        val result = controller.setIndexGuarded(requestedIndex = -3, expectedIndex = 5)

        assertEquals(SystemVolumeWriteResult.Outcome.APPLIED, result.outcome)
        assertEquals(listOf(2), backend.writes)
        assertEquals(2, backend.current)
    }

    @Test
    fun `guarded write applies a lower level with matching expected index`() {
        val backend = FakeBackend(current = 8)
        val controller = SystemVolumeController(backend)

        val result = controller.setIndexGuarded(requestedIndex = 7, expectedIndex = 8)

        assertEquals(SystemVolumeWriteResult.Outcome.APPLIED, result.outcome)
        assertEquals(7, result.appliedIndex)
        assertEquals(listOf(7), backend.writes)
    }

    @Test
    fun `guarded write reports a read-back mismatch as a failure`() {
        val backend = FakeBackend(current = 8).apply {
            applyWrite = { requested ->
                current = if (requested == 7) 6 else requested
            }
        }
        val controller = SystemVolumeController(backend)

        val result = controller.setIndexGuarded(requestedIndex = 7, expectedIndex = 8)

        assertEquals(SystemVolumeWriteResult.Outcome.FAILED, result.outcome)
        assertEquals(6, result.appliedIndex)
        assertFalse(result.isApplied)
        assertEquals(listOf(7), backend.writes)
    }

    @Test
    fun `ordinary DG-024 setIndex still does not use an expected index`() {
        val backend = FakeBackend(current = 5)
        val controller = SystemVolumeController(backend)

        // The normal slider path may raise the level; only the timer path is
        // expected-index/lower-only guarded.
        val result = controller.setIndex(7)

        assertEquals(SystemVolumeWriteResult.Outcome.APPLIED, result.outcome)
        assertEquals(listOf(7), backend.writes)
        assertEquals(7, backend.current)
    }

    @Test
    fun `barrier refuses a queued timer write after the slider invalidates it`() {
        val barrier = SleepTimerWriteBarrier()
        val gate = SleepTimerGate()
        val epoch = gate.currentEpoch()
        var manualWrites = 0

        barrier.manualWrite {
            gate.invalidate(SleepTimerOutcome.MANUAL_VOLUME_CHANGE)
            barrier.setEnabled(false)
            manualWrites += 1
        }

        val timerWrite = barrier.guardedTimerWrite(
            canWrite = { gate.currentEpoch() == epoch },
        ) { "timer-write" }

        assertNull(timerWrite)
        assertEquals(1, manualWrites)
    }

    @Test
    fun `barrier allows a timer write while enabled and the gate matches`() {
        val barrier = SleepTimerWriteBarrier()
        val gate = SleepTimerGate()
        val epoch = gate.currentEpoch()

        val timerWrite = barrier.guardedTimerWrite(
            canWrite = { gate.currentEpoch() == epoch },
        ) { "timer-write" }

        assertEquals("timer-write", timerWrite ?: "")
    }

    @Test
    fun `engine ends the timer when the guarded write reports an external change`() {
        val clock = MutableClock()
        val gate = SleepTimerGate()
        var writes = 0
        val volume = object : SleepTimerVolume {
            override fun read(): SystemVolumeSnapshot =
                SystemVolumeSnapshot(
                    isAvailable = true,
                    isFixed = false,
                    minIndex = 0,
                    maxIndex = 15,
                    currentIndex = 8,
                )

            override fun write(index: Int, expectedIndex: Int): SystemVolumeWriteResult {
                writes += 1
                return SystemVolumeWriteResult(
                    outcome = SystemVolumeWriteResult.Outcome.EXTERNAL,
                    requestedIndex = index,
                    appliedIndex = 5,
                    message = "changed outside",
                )
            }
        }
        val engine = SleepTimerEngine(volume = volume, clock = clock, gate = gate)

        assertTrue(engine.start(SleepTimerConfig(60_000L, 20_000L, 3)).accepted)
        clock.nowMs = 45_000L // started at 1_000 ms, first fade step due
        val status = engine.tick()

        assertEquals(SleepTimerOutcome.MANUAL_VOLUME_CHANGE, status.outcome)
        assertEquals(1, writes)
        assertFalse(status.isRunning)
    }
}
