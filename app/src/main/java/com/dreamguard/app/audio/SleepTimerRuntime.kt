package com.dreamguard.app.audio

/**
 * Process-wide handle shared by the visible UI and the foreground timer service.
 *
 * The in-app volume slider and the timer both write the shared media channel. The
 * slider runs on main and the timer on its serial Handler thread, so they share a
 * short [SleepTimerWriteBarrier] and a gate:
 *
 *  - a slider write runs inside the barrier, invalidates the gate and disables
 *    timer writes before its own `setStreamVolume`;
 *  - a timer write runs inside the same barrier, re-checks the gate under the
 *    lock, and the production controller takes one last expected-index snapshot
 *    immediately before the write.
 *
 * A timer write already inside `AudioManager.setStreamVolume` cannot be
 * preempted; an external hardware key or another app through the Android Binder
 * is not part of this barrier. Those non-atomic windows are documented in
 * `docs/STATUS.md` rather than hidden.
 */
object SleepTimerRuntime {
    private val gate = SleepTimerGate()
    private val writeBarrier = SleepTimerWriteBarrier()

    @Volatile
    var isActive: Boolean = false
        private set

    @Volatile
    private var service: SleepTimerService? = null

    fun gate(): SleepTimerGate = gate

    fun writeBarrier(): SleepTimerWriteBarrier = writeBarrier

    fun attach(service: SleepTimerService) {
        this.service = service
    }

    fun detach(service: SleepTimerService) {
        if (this.service === service) this.service = null
    }

    fun setActive(active: Boolean) {
        isActive = active
    }

    /** True when this process currently owns a service instance (running or idle). */
    fun hasLiveService(): Boolean = service != null

    /**
     * Runs one in-app slider write.
     *
     * The first call while a timer is active synchronously invalidates the gate,
     * disables timer writes and asks the service to persist the terminal state.
     * All of that happens **before** the write block and under the same barrier
     * as the timer's writes, so a queued old tick cannot write after this call.
     * With no active timer it only adds the short barrier and behaves exactly
     * like the DG-024 slider.
     */
    fun runInAppVolumeWrite(block: () -> SystemVolumeWriteResult): SystemVolumeWriteResult =
        writeBarrier.manualWrite {
            // Invalidate even when isActive is still false: a timer start may be
            // in flight between the timer thread and its first tick. A later
            // accepted start captures the new epoch, so this is harmless when no
            // timer exists and keeps the slider's decision authoritative.
            gate.invalidate(SleepTimerOutcome.MANUAL_VOLUME_CHANGE)
            writeBarrier.setEnabled(false)
            if (isActive) {
                isActive = false
                service?.onInAppVolumeChange()
            }
            block()
        }
}
