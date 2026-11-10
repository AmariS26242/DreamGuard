package com.dreamguard.app.audio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Host-JVM tests for the DG-025 timer rules.
 *
 * They exercise the real [SystemVolumeController] (the DG-024 read/write rules)
 * through a fake [MediaVolumeBackend], so clamping, fixed-volume detection,
 * read/write failures and silent-ignore handling are the same code paths the
 * service uses. Not covered here: the foreground service, notifications,
 * `AudioManager`, the Settings observer, the AudioDeviceCallback initial-snapshot
 * rule and real device behaviour; those are recorded as unverified in
 * `docs/STATUS.md` until a device/Gradle run exists.
 */
class SleepTimerEngineTest {

    private class FakeBackend(
        var fixed: Boolean = false,
        var min: Int = 0,
        var max: Int = 15,
        var current: Int = 8,
    ) : MediaVolumeBackend {
        val writes = mutableListOf<Int>()
        var readFailure: RuntimeException? = null
        var writeFailure: RuntimeException? = null
        var applyWrite: (Int) -> Unit = { current = it }

        override fun isVolumeFixed(): Boolean {
            failReadIfRequested()
            return fixed
        }

        override fun minIndex(): Int {
            failReadIfRequested()
            return min
        }

        override fun maxIndex(): Int {
            failReadIfRequested()
            return max
        }

        override fun currentIndex(): Int {
            failReadIfRequested()
            return current
        }

        override fun setIndex(index: Int) {
            writeFailure?.let { throw it }
            writes += index
            applyWrite(index)
        }

        private fun failReadIfRequested() {
            readFailure?.let { throw it }
        }
    }

    private class MutableClock(var nowMs: Long = 1_000L) : SleepTimerClock {
        override fun elapsedRealtimeMs(): Long = nowMs
    }

    private class Harness(
        current: Int = 8,
        min: Int = 0,
        max: Int = 15,
    ) {
        val clock = MutableClock()
        val backend = FakeBackend(current = current, min = min, max = max)
        val gate = SleepTimerGate()
        var signature: String? = "speaker"
        val engine = SleepTimerEngine(
            volume = ControllerSleepTimerVolume(SystemVolumeController(backend)),
            clock = clock,
            gate = gate,
            outputSignature = { signature },
        )

        fun start(
            totalMs: Long = 60_000L,
            fadeMs: Long = 20_000L,
            target: Int = 3,
        ): SleepTimerStartResult =
            engine.start(SleepTimerConfig(totalMs, fadeMs, target))

        fun tickAt(offsetMs: Long): SleepTimerStatus {
            clock.nowMs = START_MS + offsetMs
            return engine.tick()
        }

        companion object {
            const val START_MS = 1_000L
        }
    }

    private fun assertTerminal(status: SleepTimerStatus, outcome: SleepTimerOutcome) {
        assertEquals(outcome, status.outcome)
        assertEquals(SleepTimerPhase.FINISHED, status.phase)
        assertFalse(status.isRunning)
        assertNotNull(status.message)
    }

    // ---- start validation ------------------------------------------------

    @Test
    fun `fixed volume rejects the start without writing`() {
        val harness = Harness()
        harness.backend.fixed = true

        val result = harness.start()

        assertFalse(result.accepted)
        assertEquals(SleepTimerStartProblem.FIXED_VOLUME, result.problem)
        assertFalse(harness.engine.status.isRunning)
        assertEquals(SleepTimerStartProblem.FIXED_VOLUME, harness.engine.status.startProblem)
        assertTrue(harness.backend.writes.isEmpty())
    }

    @Test
    fun `single level output rejects the start`() {
        val harness = Harness(current = 4, min = 4, max = 4)

        val result = harness.start()

        assertEquals(SleepTimerStartProblem.SINGLE_LEVEL, result.problem)
        assertTrue(harness.backend.writes.isEmpty())
    }

    @Test
    fun `unreadable volume rejects the start`() {
        val harness = Harness()
        harness.backend.readFailure = IllegalStateException("audio service gone")

        val result = harness.start()

        assertEquals(SleepTimerStartProblem.READ_FAILED, result.problem)
        assertFalse(harness.engine.status.isRunning)
        assertTrue(harness.backend.writes.isEmpty())
    }

    @Test
    fun `target that is not lower is rejected`() {
        val harness = Harness(current = 8)

        val result = harness.start(target = 8)

        assertEquals(SleepTimerStartProblem.TARGET_NOT_LOWER, result.problem)
        assertTrue(harness.backend.writes.isEmpty())
    }

    @Test
    fun `already-minimum volume is rejected`() {
        val harness = Harness(current = 0, min = 0, max = 15)

        val result = harness.start(target = 0)

        assertEquals(SleepTimerStartProblem.CURRENT_AT_MINIMUM, result.problem)
        assertTrue(harness.backend.writes.isEmpty())
    }

    @Test
    fun `invalid durations are rejected`() {
        val tooLongFade = Harness().start(totalMs = 60_000L, fadeMs = 61_000L)
        assertEquals(SleepTimerStartProblem.INVALID_DURATION, tooLongFade.problem)

        val tooShort = Harness().start(totalMs = 5_000L, fadeMs = 0L)
        assertEquals(SleepTimerStartProblem.INVALID_DURATION, tooShort.problem)
    }

    @Test
    fun `target below the platform minimum is clamped and never written`() {
        val harness = Harness(current = 5, min = 2, max = 15)

        val result = harness.start(totalMs = 20_000L, fadeMs = 20_000L, target = -4)

        assertTrue(result.accepted)
        assertEquals(2, harness.engine.status.targetIndex)
        // One late tick is enough to reach the clamped target.
        val finished = harness.tickAt(20_000L)
        assertTerminal(finished, SleepTimerOutcome.COMPLETED)
        assertEquals(listOf(2), harness.backend.writes)
        assertTrue(harness.backend.writes.all { it >= 2 })
    }

    // ---- countdown, hold and fade ---------------------------------------

    @Test
    fun `remaining time counts down and the hold segment writes nothing`() {
        val harness = Harness(current = 8)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 20_000L, target = 3).accepted)

        val at10 = harness.tickAt(10_000L)
        assertEquals(SleepTimerPhase.HOLDING, at10.phase)
        assertEquals(50_000L, at10.remainingMs)

        val at39 = harness.tickAt(39_000L)
        assertEquals(SleepTimerPhase.HOLDING, at39.phase)
        assertEquals(21_000L, at39.remainingMs)

        assertEquals("保持段不得写入音量", emptyList<Int>(), harness.backend.writes)
    }

    @Test
    fun `fade steps down one platform level at a time and completes at target`() {
        val harness = Harness(current = 8)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 20_000L, target = 3).accepted)

        assertEquals(SleepTimerPhase.FADING, harness.tickAt(40_000L).phase)
        harness.tickAt(44_000L)
        harness.tickAt(48_000L)
        harness.tickAt(52_000L)
        harness.tickAt(56_000L)
        val finished = harness.tickAt(60_000L)

        assertEquals(listOf(7, 6, 5, 4, 3), harness.backend.writes)
        // Strictly monotonic, no duplicate level, and never below target.
        assertTrue(harness.backend.writes.zipWithNext().all { (a, b) -> b < a })
        assertEquals(3, harness.backend.writes.last())
        assertTerminal(finished, SleepTimerOutcome.COMPLETED)
        assertEquals(3, finished.lastKnownIndex)
        assertEquals(0L, finished.remainingMs)
    }

    @Test
    fun `late tick skips intermediate levels without writing out of order`() {
        val harness = Harness(current = 8)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 20_000L, target = 3).accepted)

        harness.tickAt(59_900L)
        val finished = harness.tickAt(60_000L)

        // Only the level due at each late tick is written; intermediate levels
        // are deduplicated rather than replayed out of order.
        assertEquals(listOf(4, 3), harness.backend.writes)
        assertTerminal(finished, SleepTimerOutcome.COMPLETED)
    }

    @Test
    fun `zero fade writes the target once at the end`() {
        val harness = Harness(current = 9)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 0L, target = 4).accepted)

        val at59 = harness.tickAt(59_000L)
        assertEquals(SleepTimerPhase.HOLDING, at59.phase)
        assertTrue(harness.backend.writes.isEmpty())

        val finished = harness.tickAt(60_000L)
        assertEquals(listOf(4), harness.backend.writes)
        assertTerminal(finished, SleepTimerOutcome.COMPLETED)
    }

    @Test
    fun `formatRemaining rounds up and never shows zero while running`() {
        assertEquals("01:00", formatRemaining(60_000L))
        assertEquals("00:01", formatRemaining(1L))
        assertEquals("00:01", formatRemaining(999L))
        assertEquals("00:00", formatRemaining(0L))
        assertEquals("01:02", formatRemaining(61_001L))
    }

    // ---- cancellation and manual changes --------------------------------

    @Test
    fun `cancel stops immediately and never restores the old level`() {
        val harness = Harness(current = 8)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 20_000L, target = 3).accepted)
        harness.tickAt(10_000L)

        val cancelled = harness.engine.cancel()
        assertTerminal(cancelled, SleepTimerOutcome.CANCELLED)
        assertEquals(8, harness.backend.current)

        harness.tickAt(60_000L)
        assertTrue("取消后不得再写入", harness.backend.writes.isEmpty())
        assertEquals(8, harness.backend.current)
    }

    @Test
    fun `external volume change ends the timer and keeps the user value`() {
        val harness = Harness(current = 8)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 20_000L, target = 3).accepted)
        harness.tickAt(10_000L)

        harness.backend.current = 4 // hardware volume key
        val ended = harness.tickAt(11_000L)

        assertTerminal(ended, SleepTimerOutcome.MANUAL_VOLUME_CHANGE)
        assertEquals(4, harness.backend.current)
        assertTrue(harness.backend.writes.isEmpty())
        assertTrue(ended.message.orEmpty().contains("保留"))
    }

    @Test
    fun `in-app manual change aborts before the next tick`() {
        val harness = Harness(current = 8)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 20_000L, target = 3).accepted)

        // Mirrors the UI: invalidate synchronously, then perform the slider write.
        harness.gate.invalidate(SleepTimerOutcome.MANUAL_VOLUME_CHANGE)
        harness.backend.current = 2
        val ended = harness.tickAt(10_000L)

        assertTerminal(ended, SleepTimerOutcome.MANUAL_VOLUME_CHANGE)
        assertTrue("定时器不得覆盖应用内滑块写入", harness.backend.writes.isEmpty())
        assertEquals(2, harness.backend.current)
    }

    @Test
    fun `a raised external level is never overwritten`() {
        val harness = Harness(current = 8)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 20_000L, target = 3).accepted)
        harness.tickAt(44_000L) // first fade write: 7
        assertEquals(listOf(7), harness.backend.writes)

        harness.backend.current = 9 // user raises while the fade runs
        val ended = harness.tickAt(45_000L)

        assertTerminal(ended, SleepTimerOutcome.MANUAL_VOLUME_CHANGE)
        assertEquals(9, harness.backend.current)
        assertEquals(listOf(7), harness.backend.writes)
    }

    // ---- failures and guard rails ---------------------------------------

    @Test
    fun `write failure ends with a write-failed state and no retry`() {
        val harness = Harness(current = 8)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 20_000L, target = 3).accepted)
        harness.backend.writeFailure = SecurityException("denied")

        val failed = harness.tickAt(44_000L)
        assertTerminal(failed, SleepTimerOutcome.WRITE_FAILED)
        assertEquals(8, harness.backend.current)

        harness.backend.writeFailure = null
        harness.tickAt(48_000L)
        assertTrue("失败后不得继续写入", harness.backend.writes.isEmpty())
    }

    @Test
    fun `silently ignored write is a failure not a completion`() {
        val harness = Harness(current = 8)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 20_000L, target = 3).accepted)
        harness.backend.applyWrite = { /* OEM accepts the call, keeps its level */ }

        val failed = harness.tickAt(44_000L)

        assertTerminal(failed, SleepTimerOutcome.WRITE_FAILED)
        assertEquals(listOf(7), harness.backend.writes)
        assertEquals(8, harness.backend.current)
    }

    @Test
    fun `volume becoming fixed mid-run ends the timer`() {
        val harness = Harness(current = 8)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 20_000L, target = 3).accepted)
        harness.tickAt(10_000L)

        harness.backend.fixed = true
        val ended = harness.tickAt(11_000L)

        assertTerminal(ended, SleepTimerOutcome.VOLUME_UNAVAILABLE)
        assertTrue(harness.backend.writes.isEmpty())
    }

    @Test
    fun `volume becoming single level mid-run ends the timer`() {
        val harness = Harness(current = 8)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 20_000L, target = 3).accepted)
        harness.tickAt(10_000L)

        harness.backend.min = 8
        harness.backend.max = 8
        val ended = harness.tickAt(11_000L)

        assertTerminal(ended, SleepTimerOutcome.VOLUME_UNAVAILABLE)
        assertTrue(harness.backend.writes.isEmpty())
    }

    @Test
    fun `read failure mid-run ends the timer`() {
        val harness = Harness(current = 8)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 20_000L, target = 3).accepted)
        harness.tickAt(10_000L)

        harness.backend.readFailure = IllegalStateException("gone")
        val ended = harness.tickAt(11_000L)

        assertTerminal(ended, SleepTimerOutcome.READ_FAILED)
        assertTrue(harness.backend.writes.isEmpty())
    }

    @Test
    fun `detectable output change aborts before any fade write`() {
        val harness = Harness(current = 8)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 20_000L, target = 3).accepted)

        harness.signature = "bluetooth"
        val ended = harness.tickAt(44_000L)

        assertTerminal(ended, SleepTimerOutcome.OUTPUT_CHANGED)
        assertTrue(harness.backend.writes.isEmpty())
    }

    @Test
    fun `output change also guards the final target write`() {
        val harness = Harness(current = 9)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 0L, target = 4).accepted)
        harness.tickAt(59_000L)

        harness.signature = "usb"
        val ended = harness.tickAt(60_000L)

        assertTerminal(ended, SleepTimerOutcome.OUTPUT_CHANGED)
        assertTrue(harness.backend.writes.isEmpty())
    }

    @Test
    fun `gate abort also guards the final target write`() {
        val harness = Harness(current = 9)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 0L, target = 4).accepted)
        harness.tickAt(59_000L)

        harness.gate.invalidate(SleepTimerOutcome.MANUAL_VOLUME_CHANGE)
        val ended = harness.tickAt(60_000L)

        assertTerminal(ended, SleepTimerOutcome.MANUAL_VOLUME_CHANGE)
        assertTrue(harness.backend.writes.isEmpty())
    }

    @Test
    fun `completion is not reported when the target read-back differs`() {
        val harness = Harness(current = 8)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 0L, target = 3).accepted)
        harness.backend.applyWrite = { requested ->
            harness.backend.current = if (requested == 3) 4 else requested
        }

        val ended = harness.tickAt(60_000L)

        assertNotEquals(SleepTimerOutcome.COMPLETED, ended.outcome)
        assertTerminal(ended, SleepTimerOutcome.MANUAL_VOLUME_CHANGE)
        assertEquals(4, harness.backend.current)
        assertEquals(listOf(3), harness.backend.writes)
    }

    @Test
    fun `a second start while running is rejected without disturbing the run`() {
        val harness = Harness(current = 8)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 20_000L, target = 3).accepted)
        harness.tickAt(10_000L)

        val second = harness.start(totalMs = 30_000L, fadeMs = 10_000L, target = 2)

        assertEquals(SleepTimerStartProblem.ALREADY_RUNNING, second.problem)
        assertTrue(harness.engine.status.isRunning)
        assertEquals(60_000L, harness.engine.status.totalDurationMs)
        assertNull(harness.engine.status.outcome)
    }

    @Test
    fun `stop is idempotent and repeated ticks do nothing`() {
        val harness = Harness(current = 8)
        assertTrue(harness.start(totalMs = 60_000L, fadeMs = 20_000L, target = 3).accepted)

        val first = harness.engine.stop(SleepTimerOutcome.WRITE_FAILED, "boom")
        val second = harness.engine.stop(SleepTimerOutcome.COMPLETED, "should not replace")

        assertTerminal(first, SleepTimerOutcome.WRITE_FAILED)
        assertTerminal(second, SleepTimerOutcome.WRITE_FAILED)
        harness.tickAt(60_000L)
        assertTrue(harness.backend.writes.isEmpty())
    }
}
