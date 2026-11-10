package com.dreamguard.app.audio

import android.content.Context
import android.content.SharedPreferences

/**
 * Minimal persistence for DG-025.
 *
 * Two facts survive process death:
 *  1. an "active" marker written when a run is accepted and cleared on every
 *     terminal state;
 *  2. the last terminal outcome/message.
 *
 * Each state transition is one [SleepTimerKeyValueStore.transact] call, so a
 * process kill cannot leave a half-written mix such as `active=false` with the
 * previous terminal still missing. [SleepTimerRecovery.markActive] is durable
 * (a single `commit`) and is called on the timer thread *before* the first volume
 * write; terminal writes use `apply` so the main thread never waits on disk.
 *
 * The recovery rules are pure Kotlin and JVM-tested; [SleepTimerStore] is only
 * the SharedPreferences adapter.
 */

/** One atomic edit session; all mutations are applied together. */
interface SleepTimerTransaction {
    fun putBoolean(key: String, value: Boolean)
    fun putLong(key: String, value: Long)
    fun putInt(key: String, value: Int)
    fun putString(key: String, value: String?)
    fun remove(key: String)
}

interface SleepTimerKeyValueStore {
    fun getBoolean(key: String, defaultValue: Boolean): Boolean
    fun getLong(key: String, defaultValue: Long): Long
    fun getInt(key: String, defaultValue: Int): Int
    fun getString(key: String, defaultValue: String?): String?

    /**
     * Applies [block] as one edit. [durable] uses `commit` (the caller is on a
     * worker thread); otherwise `apply` is used so no main-thread disk wait.
     *
     * @return false when the edit could not be applied.
     */
    fun transact(durable: Boolean, block: (SleepTimerTransaction) -> Unit): Boolean
}

data class SleepTimerTerminalRecord(
    val outcome: SleepTimerOutcome,
    val message: String,
    val updatedAtMs: Long,
    val totalDurationMs: Long = 0L,
    val targetIndex: Int? = null,
)

class SleepTimerRecovery(private val store: SleepTimerKeyValueStore) {

    /**
     * Marks a run as active before any volume write. A false return means the
     * marker is not recoverable and the caller must not start the timer.
     */
    fun markActive(config: SleepTimerConfig): Boolean = runCatching {
        store.transact(durable = true) { tx ->
            tx.putBoolean(KEY_ACTIVE, true)
            tx.putLong(KEY_TOTAL, config.totalDurationMs)
            tx.putInt(KEY_TARGET, config.targetIndex)
            tx.remove(KEY_TERMINAL)
            tx.remove(KEY_MESSAGE)
            tx.remove(KEY_UPDATED_AT)
        }
    }.getOrDefault(false)

    fun markTerminal(
        outcome: SleepTimerOutcome,
        message: String,
        updatedAtMs: Long,
        config: SleepTimerConfig? = null,
    ) {
        runCatching {
            store.transact(durable = false) { tx ->
                tx.putBoolean(KEY_ACTIVE, false)
                if (config != null) {
                    tx.putLong(KEY_TOTAL, config.totalDurationMs)
                    tx.putInt(KEY_TARGET, config.targetIndex)
                }
                tx.putString(KEY_TERMINAL, outcome.name)
                tx.putString(KEY_MESSAGE, message)
                tx.putLong(KEY_UPDATED_AT, updatedAtMs)
            }
        }
    }

    /**
     * Called once when a new process starts the UI/service, before any timer
     * exists. [liveTimerActive] must be true when this process already owns a
     * running timer/service, so a normal Activity recreation cannot be mistaken
     * for a death.
     */
    fun recoverIfInterrupted(
        liveTimerActive: Boolean,
        nowMs: Long,
    ): SleepTimerTerminalRecord? {
        if (liveTimerActive) return null
        if (!store.getBoolean(KEY_ACTIVE, false)) return null

        val record = SleepTimerTerminalRecord(
            outcome = SleepTimerOutcome.INTERRUPTED,
            message = "上次睡眠定时在进程结束或服务被系统停止前未完成，已中断；" +
                "没有恢复旧音量，也没有继续写入。",
            updatedAtMs = nowMs,
            totalDurationMs = store.getLong(KEY_TOTAL, 0L),
            targetIndex = store.getInt(KEY_TARGET, -1).takeIf { it >= 0 },
        )
        runCatching {
            store.transact(durable = false) { tx ->
                tx.putBoolean(KEY_ACTIVE, false)
                tx.putString(KEY_TERMINAL, record.outcome.name)
                tx.putString(KEY_MESSAGE, record.message)
                tx.putLong(KEY_UPDATED_AT, record.updatedAtMs)
            }
        }
        return record
    }

    fun lastTerminal(): SleepTimerTerminalRecord? {
        val name = store.getString(KEY_TERMINAL, null) ?: return null
        val outcome = runCatching { SleepTimerOutcome.valueOf(name) }.getOrNull() ?: return null
        return SleepTimerTerminalRecord(
            outcome = outcome,
            message = store.getString(KEY_MESSAGE, null).orEmpty(),
            updatedAtMs = store.getLong(KEY_UPDATED_AT, 0L),
            totalDurationMs = store.getLong(KEY_TOTAL, 0L),
            targetIndex = store.getInt(KEY_TARGET, -1).takeIf { it >= 0 },
        )
    }

    /** True while a persisted run has not reached a terminal state. */
    fun isActiveMarked(): Boolean = store.getBoolean(KEY_ACTIVE, false)

    fun clearTerminal() {
        runCatching {
            store.transact(durable = false) { tx ->
                tx.remove(KEY_TERMINAL)
                tx.remove(KEY_MESSAGE)
                tx.remove(KEY_UPDATED_AT)
            }
        }
    }

    private companion object {
        const val KEY_ACTIVE = "active"
        const val KEY_TERMINAL = "terminal"
        const val KEY_MESSAGE = "message"
        const val KEY_UPDATED_AT = "updated_at"
        const val KEY_TOTAL = "total_ms"
        const val KEY_TARGET = "target_index"
    }
}

class SleepTimerStore(context: Context) : SleepTimerKeyValueStore {
    private val preferences: SharedPreferences =
        context.applicationContext.getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE)

    override fun getBoolean(key: String, defaultValue: Boolean): Boolean =
        preferences.getBoolean(key, defaultValue)

    override fun getLong(key: String, defaultValue: Long): Long =
        preferences.getLong(key, defaultValue)

    override fun getInt(key: String, defaultValue: Int): Int =
        preferences.getInt(key, defaultValue)

    override fun getString(key: String, defaultValue: String?): String? =
        preferences.getString(key, defaultValue)

    override fun transact(durable: Boolean, block: (SleepTimerTransaction) -> Unit): Boolean {
        val editor = preferences.edit()
        val transaction = object : SleepTimerTransaction {
            override fun putBoolean(key: String, value: Boolean) {
                editor.putBoolean(key, value)
            }

            override fun putLong(key: String, value: Long) {
                editor.putLong(key, value)
            }

            override fun putInt(key: String, value: Int) {
                editor.putInt(key, value)
            }

            override fun putString(key: String, value: String?) {
                editor.putString(key, value)
            }

            override fun remove(key: String) {
                editor.remove(key)
            }
        }
        return try {
            block(transaction)
            if (durable) {
                editor.commit()
            } else {
                editor.apply()
                true
            }
        } catch (_: RuntimeException) {
            false
        }
    }

    private companion object {
        const val PREFERENCES_NAME = "dreamguard.sleep_timer"
    }
}
