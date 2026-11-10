package com.dreamguard.app.audio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Host-JVM tests for the system media volume rules (DG-024).
 *
 * These cover what can be decided without Android: index ranges and clamping,
 * fixed-volume and single-level outputs, read/write failures, a platform that
 * silently ignores a write, external (hardware-key/other-app) changes, and the
 * "publish only real changes" rule that keeps the Compose card from churning.
 *
 * Not covered here: the real `AudioManager` values, the Settings ContentObserver,
 * Activity lifecycle registration, and the device behaviour of a third-party
 * player (those need a device/emulator and are recorded in docs/STATUS.md as
 * unverified until actually run).
 */
class SystemMediaVolumeTest {

    private class FakeBackend(
        var fixed: Boolean = false,
        var min: Int = 0,
        var max: Int = 15,
        var current: Int = 7,
    ) : MediaVolumeBackend {
        val writes = mutableListOf<Int>()
        var readFailure: RuntimeException? = null
        var writeFailure: RuntimeException? = null

        /** A test can replace this to model an OEM that ignores the write. */
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

    @Test
    fun `read reports the platform range and level`() {
        val snapshot = SystemVolumeController(FakeBackend()).read()

        assertTrue(snapshot.isAvailable)
        assertFalse(snapshot.isFixed)
        assertTrue(snapshot.isAdjustable)
        assertEquals(0, snapshot.minIndex)
        assertEquals(15, snapshot.maxIndex)
        assertEquals(7, snapshot.currentIndex)
        assertEquals(47, snapshot.percent)
        assertNull(snapshot.readError)
    }

    @Test
    fun `read clamps a level the platform reports outside its own range`() {
        val backend = FakeBackend(current = 99)
        val controller = SystemVolumeController(backend)

        assertEquals(15, controller.read().currentIndex)

        backend.current = -4
        assertEquals(0, controller.read().currentIndex)
    }

    @Test
    fun `write clamps to the platform minimum and maximum`() {
        val backend = FakeBackend()
        val controller = SystemVolumeController(backend)

        val atMin = controller.setIndex(-3)
        assertTrue(atMin.isApplied)
        assertEquals(0, atMin.appliedIndex)

        val atMax = controller.setIndex(99)
        assertTrue(atMax.isApplied)
        assertEquals(15, atMax.appliedIndex)

        assertEquals(listOf(0, 15), backend.writes)
        assertEquals(15, controller.read().currentIndex)
    }

    @Test
    fun `fixed-volume device is shown read-only and never written`() {
        val backend = FakeBackend(fixed = true, current = 8)
        val controller = SystemVolumeController(backend)

        val snapshot = controller.read()
        assertTrue(snapshot.isAvailable)
        assertTrue(snapshot.isFixed)
        assertFalse(snapshot.isAdjustable)
        assertEquals(8, snapshot.currentIndex)

        val result = controller.setIndex(3)
        assertEquals(SystemVolumeWriteResult.Outcome.FIXED, result.outcome)
        assertEquals(8, result.appliedIndex)
        assertNotNull(result.message)
        assertTrue(backend.writes.isEmpty())
    }

    @Test
    fun `single-level output is available but not adjustable`() {
        val backend = FakeBackend(min = 4, max = 4, current = 4)
        val controller = SystemVolumeController(backend)

        val snapshot = controller.read()
        assertTrue(snapshot.isAvailable)
        assertFalse(snapshot.isFixed)
        assertFalse(snapshot.isAdjustable)

        val result = controller.setIndex(4)
        assertEquals(SystemVolumeWriteResult.Outcome.UNAVAILABLE, result.outcome)
        assertTrue(backend.writes.isEmpty())
        assertNotNull(result.message)
    }

    @Test
    fun `read failure yields an unavailable state instead of crashing`() {
        val backend = FakeBackend().apply {
            readFailure = IllegalStateException("audio service gone")
        }
        val controller = SystemVolumeController(backend)

        val snapshot = controller.read()
        assertFalse(snapshot.isAvailable)
        assertFalse(snapshot.isAdjustable)
        assertNotNull(snapshot.readError)

        val result = controller.setIndex(5)
        assertEquals(SystemVolumeWriteResult.Outcome.UNAVAILABLE, result.outcome)
        assertTrue(backend.writes.isEmpty())
        assertNotNull(result.message)
    }

    @Test
    fun `rejected write keeps the system value and surfaces a message`() {
        val backend = FakeBackend(current = 7).apply {
            writeFailure = SecurityException("denied")
        }
        val controller = SystemVolumeController(backend)

        val result = controller.setIndex(12)
        assertEquals(SystemVolumeWriteResult.Outcome.FAILED, result.outcome)
        assertEquals(7, result.appliedIndex)
        assertTrue(result.message.orEmpty().contains("失败"))
        assertTrue(backend.writes.isEmpty())
        assertEquals(7, controller.read().currentIndex)
    }

    @Test
    fun `silently ignored write is not reported as success`() {
        val backend = FakeBackend(current = 7).apply {
            // The call succeeds but the platform keeps its own level.
            applyWrite = {}
        }
        val controller = SystemVolumeController(backend)

        val result = controller.setIndex(3)
        assertEquals(SystemVolumeWriteResult.Outcome.FAILED, result.outcome)
        assertEquals(7, result.appliedIndex)
        assertTrue(result.message.orEmpty().contains("未应用"))
        assertEquals(listOf(3), backend.writes)
    }

    @Test
    fun `external change publishes once and no-op refreshes stay quiet`() {
        val backend = FakeBackend(current = 7)
        val controller = SystemVolumeController(backend)
        val seen = mutableListOf<SystemVolumeSnapshot>()
        controller.listener = { seen += it }

        controller.refresh() // initial publish
        backend.current = 2 // e.g. hardware volume key
        controller.refresh()
        controller.refresh() // same value, must not publish again

        assertEquals(2, seen.size)
        assertEquals(2, seen.last().currentIndex)
    }

    @Test
    fun `successful write publishes the applied level`() {
        val backend = FakeBackend(current = 7)
        val controller = SystemVolumeController(backend)
        val seen = mutableListOf<SystemVolumeSnapshot>()
        controller.listener = { seen += it }

        controller.refresh()
        val result = controller.setIndex(12)

        assertTrue(result.isApplied)
        assertEquals(12, result.appliedIndex)
        assertEquals(2, seen.size)
        assertEquals(12, seen.last().currentIndex)
    }

    @Test
    fun `failed write does not publish a fake level`() {
        val backend = FakeBackend(current = 7).apply {
            writeFailure = IllegalStateException("rejected")
        }
        val controller = SystemVolumeController(backend)
        val seen = mutableListOf<SystemVolumeSnapshot>()
        controller.listener = { seen += it }

        controller.refresh()
        val result = controller.setIndex(12)

        assertEquals(SystemVolumeWriteResult.Outcome.FAILED, result.outcome)
        // The system still holds 7, so no visible state changed and the card
        // keeps showing the real level while the result carries the message.
        assertEquals(1, seen.size)
        assertEquals(7, seen.single().currentIndex)
    }

    @Test
    fun `unavailable state recovers when the platform becomes readable`() {
        val backend = FakeBackend(current = 9).apply {
            readFailure = IllegalStateException("temporarily unavailable")
        }
        val controller = SystemVolumeController(backend)
        val seen = mutableListOf<SystemVolumeSnapshot>()
        controller.listener = { seen += it }

        controller.refresh()
        assertFalse(seen.single().isAvailable)

        backend.readFailure = null
        controller.refresh()

        assertEquals(2, seen.size)
        assertTrue(seen.last().isAvailable)
        assertEquals(9, seen.last().currentIndex)
    }
}
