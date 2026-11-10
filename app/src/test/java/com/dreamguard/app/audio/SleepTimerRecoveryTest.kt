package com.dreamguard.app.audio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * JVM tests for the persisted "active marker / last terminal" rules.
 *
 * They model process death: the marker survives, the in-memory timer does not.
 * Recovery is asserted to produce an explicit interrupted record and to touch no
 * volume state at all (there is no volume dependency in this class). The local
 * store applies each transition to a scratch map and commits it once, so a
 * failed/aborted transaction can be tested without Android.
 */
class SleepTimerRecoveryTest {

    private class MapStore : SleepTimerKeyValueStore {
        private val values = mutableMapOf<String, Any?>()
        var transactionCount = 0
        var failDurableTransactions = false

        override fun getBoolean(key: String, defaultValue: Boolean): Boolean =
            values[key] as? Boolean ?: defaultValue

        override fun getLong(key: String, defaultValue: Long): Long =
            values[key] as? Long ?: defaultValue

        override fun getInt(key: String, defaultValue: Int): Int =
            values[key] as? Int ?: defaultValue

        override fun getString(key: String, defaultValue: String?): String? =
            if (values.containsKey(key)) values[key] as? String else defaultValue

        override fun transact(
            durable: Boolean,
            block: (SleepTimerTransaction) -> Unit,
        ): Boolean {
            transactionCount += 1
            if (durable && failDurableTransactions) return false
            val scratch = values.toMutableMap()
            val transaction = object : SleepTimerTransaction {
                override fun putBoolean(key: String, value: Boolean) {
                    scratch[key] = value
                }

                override fun putLong(key: String, value: Long) {
                    scratch[key] = value
                }

                override fun putInt(key: String, value: Int) {
                    scratch[key] = value
                }

                override fun putString(key: String, value: String?) {
                    if (value == null) scratch.remove(key) else scratch[key] = value
                }

                override fun remove(key: String) {
                    scratch.remove(key)
                }
            }
            return try {
                block(transaction)
                values.clear()
                values.putAll(scratch)
                true
            } catch (_: RuntimeException) {
                false
            }
        }
    }

    @Test
    fun `new process turns the active marker into an interrupted prompt`() {
        val store = MapStore()
        val recovery = SleepTimerRecovery(store)
        assertTrue(recovery.markActive(SleepTimerConfig(60_000L, 20_000L, 3)))

        assertTrue(recovery.isActiveMarked())
        val record = recovery.recoverIfInterrupted(liveTimerActive = false, nowMs = 9_999L)

        assertNotNull(record)
        val actualRecord = record!!
        assertEquals(SleepTimerOutcome.INTERRUPTED, actualRecord.outcome)
        assertTrue(actualRecord.message.contains("没有恢复旧音量"))
        assertFalse(recovery.isActiveMarked())
        assertEquals(SleepTimerOutcome.INTERRUPTED, recovery.lastTerminal()?.outcome)

        // Recovery is one-shot: a second call must not create a second prompt.
        assertNull(recovery.recoverIfInterrupted(liveTimerActive = false, nowMs = 10_000L))
    }

    @Test
    fun `live service suppresses recovery and keeps the marker`() {
        val store = MapStore()
        val recovery = SleepTimerRecovery(store)
        assertTrue(recovery.markActive(SleepTimerConfig(30_000L, 10_000L, 2)))

        assertNull(recovery.recoverIfInterrupted(liveTimerActive = true, nowMs = 999L))
        assertTrue(recovery.isActiveMarked())
        assertNull(recovery.lastTerminal())
    }

    @Test
    fun `terminal state clears the active marker and blocks recovery`() {
        val store = MapStore()
        val recovery = SleepTimerRecovery(store)
        assertTrue(recovery.markActive(SleepTimerConfig(60_000L, 20_000L, 3)))
        recovery.markTerminal(SleepTimerOutcome.COMPLETED, "done", 2_000L)

        assertFalse(recovery.isActiveMarked())
        assertNull(recovery.recoverIfInterrupted(liveTimerActive = false, nowMs = 3_000L))
        val terminal = recovery.lastTerminal()
        assertNotNull(terminal)
        val actual = terminal!!
        assertEquals(SleepTimerOutcome.COMPLETED, actual.outcome)
        assertEquals("done", actual.message)
    }

    @Test
    fun `starting a new run clears the previous terminal record`() {
        val store = MapStore()
        val recovery = SleepTimerRecovery(store)
        recovery.markTerminal(SleepTimerOutcome.CANCELLED, "old", 1L)
        assertNotNull(recovery.lastTerminal())

        assertTrue(recovery.markActive(SleepTimerConfig(60_000L, 0L, 4)))

        assertNull(recovery.lastTerminal())
        assertTrue(recovery.isActiveMarked())
    }

    @Test
    fun `terminal record keeps duration and target context`() {
        val store = MapStore()
        val recovery = SleepTimerRecovery(store)
        assertTrue(recovery.markActive(SleepTimerConfig(90_000L, 30_000L, 5)))
        recovery.markTerminal(SleepTimerOutcome.CANCELLED, "cancelled", 20L)

        val terminal = recovery.lastTerminal()
        assertNotNull(terminal)
        val actual = terminal!!
        assertEquals(90_000L, actual.totalDurationMs)
        assertEquals(5, actual.targetIndex ?: -1)
        assertEquals(20L, actual.updatedAtMs)
    }

    @Test
    fun `clearTerminal leaves the active marker untouched`() {
        val store = MapStore()
        val recovery = SleepTimerRecovery(store)
        assertTrue(recovery.markActive(SleepTimerConfig(60_000L, 20_000L, 3)))

        recovery.clearTerminal()

        assertTrue(recovery.isActiveMarked())
        assertNull(recovery.lastTerminal())
    }

    @Test
    fun `active marker is one durable transaction`() {
        val store = MapStore()
        val recovery = SleepTimerRecovery(store)

        assertTrue(recovery.markActive(SleepTimerConfig(60_000L, 20_000L, 3)))

        assertEquals(1, store.transactionCount)
        assertTrue(recovery.isActiveMarked())
    }

    @Test
    fun `failed durable active write leaves no marker`() {
        val store = MapStore().apply { failDurableTransactions = true }
        val recovery = SleepTimerRecovery(store)

        assertFalse(recovery.markActive(SleepTimerConfig(60_000L, 20_000L, 3)))

        assertFalse(recovery.isActiveMarked())
        assertNull(recovery.lastTerminal())
    }

    @Test
    fun `terminal transition is one transaction`() {
        val store = MapStore()
        val recovery = SleepTimerRecovery(store)
        assertTrue(recovery.markActive(SleepTimerConfig(60_000L, 20_000L, 3)))
        val afterActive = store.transactionCount

        recovery.markTerminal(SleepTimerOutcome.CANCELLED, "cancelled", 20L)

        assertEquals(afterActive + 1, store.transactionCount)
        assertFalse(recovery.isActiveMarked())
        assertEquals(SleepTimerOutcome.CANCELLED, recovery.lastTerminal()?.outcome)
    }
}
