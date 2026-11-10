package com.dreamguard.app.audio

import java.util.concurrent.locks.ReentrantLock

/**
 * Short process-wide barrier shared by the in-app DG-024 slider and the DG-025
 * timer writes.
 *
 * Why it exists: the timer writes on its own Handler thread while the visible
 * slider writes on main. The timer's gate is checked before a write, but a tick
 * that is already inside `setStreamVolume` cannot be preempted. This barrier makes
 * the two writes take turns:
 *
 *  - a manual slider write holds the lock while it runs;
 *  - a timer write may proceed only while [enabled] and its `canWrite` check still
 *    hold *inside* the same lock.
 *
 * So once the slider has invalidated the timer's gate and disabled timer writes,
 * a queued timer write waits for the lock and then aborts instead of overwriting
 * the user's value. An external hardware key or another app writing through the
 * Android Binder is not part of this lock; that unavoidable non-atomic window is
 * documented separately.
 */
class SleepTimerWriteBarrier {
    private val lock = ReentrantLock()
    private var enabled = true

    fun setEnabled(enabled: Boolean) {
        lock.lock()
        try {
            this.enabled = enabled
        } finally {
            lock.unlock()
        }
    }

    fun isEnabled(): Boolean {
        lock.lock()
        try {
            return enabled
        } finally {
            lock.unlock()
        }
    }

    /** Runs one in-app slider write while excluding timer writes. */
    fun <T> manualWrite(block: () -> T): T {
        lock.lock()
        try {
            return block()
        } finally {
            lock.unlock()
        }
    }

    /**
     * Runs one guarded timer write.
     *
     * @return the block result, or null when timer writes are disabled or
     * [canWrite] returns false under the lock.
     */
    fun <T> guardedTimerWrite(canWrite: () -> Boolean, block: () -> T): T? {
        lock.lock()
        try {
            if (!enabled || !canWrite()) return null
            return block()
        } finally {
            lock.unlock()
        }
    }
}
