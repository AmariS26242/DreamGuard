package com.dreamguard.app.audio

/**
 * DG-025 countdown/fade state machine.
 *
 * Purely Kotlin: no Android type, no thread, no Handler. The foreground service
 * owns one instance and calls every method on its single serial timer thread, so
 * "start", "tick", "cancel" and every volume write are serialized. [SleepTimerGate]
 * additionally lets the visible in-app volume slider abort a run *synchronously*
 * before it performs its own write, without waiting for the next tick.
 */

fun interface SleepTimerClock {
    fun elapsedRealtimeMs(): Long
}

/** Narrow port over DG-024's [SystemVolumeController]; a fake is used in JVM tests. */
interface SleepTimerVolume {
    fun read(): SystemVolumeSnapshot

    /**
     * Guarded write. [expectedIndex] is the index the timer last observed; the
     * production adapter passes it to [SystemVolumeController.setIndexGuarded] so
     * a value changed after the tick's read is never overwritten.
     */
    fun write(index: Int, expectedIndex: Int): SystemVolumeWriteResult
}

class ControllerSleepTimerVolume(
    private val controller: SystemVolumeController,
) : SleepTimerVolume {
    override fun read(): SystemVolumeSnapshot = controller.read()

    override fun write(index: Int, expectedIndex: Int): SystemVolumeWriteResult =
        controller.setIndexGuarded(index, expectedIndex)
}

/**
 * Process-wide "do not write any more" epoch.
 *
 * The in-app slider invalidates it on the main thread immediately before its own
 * write. The timer checks the epoch again immediately before every guarded write
 * (including the final target write), so a tick that has not yet reached its write
 * sees the invalidation and aborts. A write that is already inside
 * `AudioManager.setStreamVolume` cannot be preempted; that non-atomic window is a
 * documented platform limitation, not something this class can close.
 */
class SleepTimerGate {
    private val lock = Any()
    private var epoch: Long = 0L
    private var lastReason: SleepTimerOutcome = SleepTimerOutcome.MANUAL_VOLUME_CHANGE

    fun currentEpoch(): Long = synchronized(lock) { epoch }

    fun lastReason(): SleepTimerOutcome = synchronized(lock) { lastReason }

    fun invalidate(reason: SleepTimerOutcome): Long = synchronized(lock) {
        lastReason = reason
        epoch += 1L
        epoch
    }
}

data class SleepTimerStartResult(
    val accepted: Boolean,
    val problem: SleepTimerStartProblem? = null,
    val message: String? = null,
)

/**
 * Time is measured with the caller-supplied monotonic clock (production:
 * `SystemClock.elapsedRealtime()`), so a wall-clock change cannot stretch or
 * shorten a countdown.
 */
class SleepTimerEngine(
    private val volume: SleepTimerVolume,
    private val clock: SleepTimerClock,
    private val gate: SleepTimerGate,
    /** Shared with the in-app slider in production; a private one is fine in tests. */
    private val writeBarrier: SleepTimerWriteBarrier = SleepTimerWriteBarrier(),
    /** Empty/failed signature means "cannot detect output changes", not "none". */
    private val outputSignature: () -> String? = { null },
) {
    var status: SleepTimerStatus = SleepTimerStatus()
        private set

    private var startedAtMs = 0L
    private var totalMs = 0L
    private var holdMs = 0L
    private var fadeMs = 0L
    private var startIndex = 0
    private var expectedIndex = 0
    private var targetIndex = 0
    private var minIndex = 0
    private var maxIndex = 0
    private var steps = 0
    private var startEpoch = 0L
    private var baselineSignature: String? = null
    private var writtenIndex: Int? = null
    private var finished = true

    /**
     * Validates and arms a run. Rejections produce an explicit [SleepTimerStatus]
     * with [SleepTimerStatus.startProblem] and never leave a fake running state.
     */
    fun start(
        config: SleepTimerConfig,
        nowMs: Long = clock.elapsedRealtimeMs(),
    ): SleepTimerStartResult {
        if (status.isRunning) {
            return SleepTimerStartResult(
                accepted = false,
                problem = SleepTimerStartProblem.ALREADY_RUNNING,
                message = SleepTimerPolicy.startProblemMessage(SleepTimerStartProblem.ALREADY_RUNNING),
            )
        }
        val snapshot = readSnapshot()
        if (snapshot == null) {
            return reject(SleepTimerStartProblem.READ_FAILED, config)
        }
        val problem = SleepTimerPolicy.validate(config, snapshot)
        if (problem != null) {
            return reject(problem, config)
        }

        startedAtMs = nowMs
        totalMs = config.totalDurationMs
        fadeMs = config.fadeDurationMs.coerceIn(0L, totalMs)
        holdMs = totalMs - fadeMs
        startIndex = snapshot.currentIndex
        expectedIndex = startIndex
        targetIndex = SleepTimerPolicy.clampTarget(config.targetIndex, snapshot)
        minIndex = snapshot.minIndex
        maxIndex = snapshot.maxIndex
        steps = startIndex - targetIndex
        startEpoch = gate.currentEpoch()
        baselineSignature = runCatching { outputSignature() }.getOrNull()
        writtenIndex = null
        finished = false
        status = SleepTimerStatus(
            phase = phaseAt(0L),
            config = config.copy(targetIndex = targetIndex),
            startedAtMs = startedAtMs,
            totalDurationMs = totalMs,
            holdDurationMs = holdMs,
            fadeDurationMs = fadeMs,
            remainingMs = totalMs,
            startIndex = startIndex,
            lastKnownIndex = expectedIndex,
            targetIndex = targetIndex,
            minIndex = minIndex,
            maxIndex = maxIndex,
        )
        return SleepTimerStartResult(accepted = true)
    }

    /**
     * One serialized step. Safe to call more or less often than once per second:
     * a late/extra tick recomputes the level due now, so a skipped intermediate
     * level is deduplicated naturally and never written out of order.
     */
    fun tick(nowMs: Long = clock.elapsedRealtimeMs()): SleepTimerStatus {
        if (finished) return status

        if (gate.currentEpoch() != startEpoch) {
            return finish(gate.lastReason(), gateMessage(gate.lastReason()))
        }

        val elapsed = (nowMs - startedAtMs).coerceAtLeast(0L)
        val snapshot = readSnapshot()
        if (snapshot == null || !snapshot.isAvailable) {
            return finish(
                SleepTimerOutcome.READ_FAILED,
                "无法读取系统媒体音量，定时已结束；音量保持当前值。",
            )
        }
        if (snapshot.isFixed) {
            return finish(
                SleepTimerOutcome.VOLUME_UNAVAILABLE,
                "媒体音量已变为固定输出，无法继续渐弱，定时已结束。",
            )
        }
        if (snapshot.maxIndex <= snapshot.minIndex) {
            return finish(
                SleepTimerOutcome.VOLUME_UNAVAILABLE,
                "当前输出只剩一个音量级别，无法继续渐弱，定时已结束。",
            )
        }
        if (snapshot.currentIndex != expectedIndex) {
            // External volume key, another app, or the in-app slider. The user's
            // value wins: end here and never write a "restore" level.
            expectedIndex = snapshot.currentIndex
            return finish(
                SleepTimerOutcome.MANUAL_VOLUME_CHANGE,
                "检测到手动音量调节（当前 ${percentOf(snapshot.currentIndex, snapshot)}%），" +
                    "定时已结束并保留该音量。",
            )
        }

        if (elapsed < holdMs) {
            return publish(nowMs)
        }

        val due = dueLevel(elapsed)
        if (due < expectedIndex && due >= targetIndex) {
            // Guard every write, including the final target write: a detectable
            // route change must never push the old target onto a new output.
            val signature = runCatching { outputSignature() }.getOrNull()
            if (signature != null && baselineSignature != null && signature != baselineSignature) {
                return finish(
                    SleepTimerOutcome.OUTPUT_CHANGED,
                    "检测到音频输出变化，定时已结束；未向新输出写入旧目标音量。",
                )
            }
            if (gate.currentEpoch() != startEpoch) {
                return finish(gate.lastReason(), gateMessage(gate.lastReason()))
            }
            when (val write = guardedWrite(due, snapshot)) {
                WriteOutcome.Applied, WriteOutcome.Skipped -> Unit
                WriteOutcome.Aborted ->
                    return finish(gate.lastReason(), gateMessage(gate.lastReason()))
                is WriteOutcome.External ->
                    return finish(
                        SleepTimerOutcome.MANUAL_VOLUME_CHANGE,
                        "写入前检测到系统音量已被外部改变（当前 ${percentOf(write.appliedIndex, snapshot)}%），" +
                            "定时已结束并保留该音量。",
                    )
                is WriteOutcome.Refused ->
                    return finish(
                        SleepTimerOutcome.MANUAL_VOLUME_CHANGE,
                        "当前音量已不高于待写级别（当前 ${percentOf(write.appliedIndex, snapshot)}%），" +
                            "定时结束并保留当前音量。",
                    )
                is WriteOutcome.Failed ->
                    return finish(
                        SleepTimerOutcome.WRITE_FAILED,
                        "写入系统媒体音量失败：${write.message ?: "系统拒绝"}；" +
                            "定时已结束，未恢复旧音量。",
                    )
                is WriteOutcome.Unavailable ->
                    return finish(
                        SleepTimerOutcome.VOLUME_UNAVAILABLE,
                        write.message ?: "媒体音量不可调，定时已结束。",
                    )
            }
        }

        if (elapsed >= totalMs) {
            // Completion is only reported after a fresh read-back equals the
            // target; "we wrote it" is not enough.
            val confirmation = readSnapshot()
            if (confirmation == null || !confirmation.isAvailable) {
                return finish(
                    SleepTimerOutcome.READ_FAILED,
                    "无法确认目标音量是否生效，定时已结束；音量保持当前值。",
                )
            }
            if (confirmation.currentIndex != targetIndex) {
                expectedIndex = confirmation.currentIndex
                return finish(
                    SleepTimerOutcome.MANUAL_VOLUME_CHANGE,
                    "完成前回读不是目标音量（当前 ${percentOf(confirmation.currentIndex, confirmation)}%），" +
                        "定时结束并保留实际音量。",
                )
            }
            return finish(
                SleepTimerOutcome.COMPLETED,
                "已降到目标音量 ${percentOf(targetIndex, confirmation)}%，定时完成。",
            )
        }

        return publish(nowMs)
    }

    /** Explicit cancellation: stops immediately and never restores the old level. */
    fun cancel(): SleepTimerStatus =
        stop(SleepTimerOutcome.CANCELLED, "已取消睡眠定时；音量保持在当前值，不恢复旧音量。")

    /**
     * Stops a run for a reason decided outside a normal tick (in-app slider,
     * output callback, foreground-service failure, service teardown).
     */
    fun stop(outcome: SleepTimerOutcome, message: String): SleepTimerStatus =
        if (finished) status else finish(outcome, message)

    // ---- internals -------------------------------------------------------

    private fun reject(problem: SleepTimerStartProblem, config: SleepTimerConfig): SleepTimerStartResult {
        val message = SleepTimerPolicy.startProblemMessage(problem)
        status = SleepTimerStatus(
            phase = SleepTimerPhase.IDLE,
            config = config,
            message = message,
            startProblem = problem,
        )
        return SleepTimerStartResult(accepted = false, problem = problem, message = message)
    }

    private fun readSnapshot(): SystemVolumeSnapshot? = try {
        volume.read()
    } catch (_: RuntimeException) {
        null
    }

    private fun phaseAt(elapsed: Long): SleepTimerPhase =
        if (elapsed < holdMs) SleepTimerPhase.HOLDING else SleepTimerPhase.FADING

    /**
     * Level due at [elapsed]. During the hold this equals the start level; during
     * the fade it walks the platform's integer indices (`start - 1`, `start - 2`,
     * ...), so successive writes are already deduplicated and monotonic.
     */
    private fun dueLevel(elapsed: Long): Int {
        if (fadeMs <= 0L) {
            return if (elapsed >= totalMs) targetIndex else startIndex
        }
        val fadeElapsed = (elapsed - holdMs).coerceIn(0L, fadeMs)
        val dueSteps = ((fadeElapsed * steps) / fadeMs).toInt().coerceIn(0, steps)
        return (startIndex - dueSteps).coerceAtLeast(targetIndex)
    }

    /**
     * A single guarded write.
     *
     * @param before the snapshot read at the start of this tick.
     *
     * The tick read, the guarded write and the controller's last pre-write
     * snapshot are still separate Android calls. An external hardware-key or
     * Binder write landing between the controller's snapshot and
     * `setStreamVolume` cannot be made atomic by the public API; that window is
     * documented, not hidden. The timer/in-app writes are serialized by the
     * shared [SleepTimerWriteBarrier].
     */
    private fun guardedWrite(level: Int, before: SystemVolumeSnapshot): WriteOutcome {
        // Never raise the shared channel, and never go below the configured target.
        if (level >= before.currentIndex) return WriteOutcome.Skipped
        if (level < targetIndex) return WriteOutcome.Skipped
        if (gate.currentEpoch() != startEpoch) return WriteOutcome.Aborted

        // The shared barrier serializes this write with an in-app slider write and
        // re-checks the gate under the same lock. The production controller then
        // takes one last system snapshot inside setIndexGuarded.
        val maybeResult = try {
            writeBarrier.guardedTimerWrite(
                canWrite = { gate.currentEpoch() == startEpoch },
            ) {
                volume.write(level, expectedIndex)
            }
        } catch (failure: RuntimeException) {
            return WriteOutcome.Failed(failure.message)
        }
        val guarded = maybeResult ?: return WriteOutcome.Aborted

        return when (guarded.outcome) {
            SystemVolumeWriteResult.Outcome.APPLIED -> {
                if (guarded.appliedIndex == level) {
                    expectedIndex = level
                    writtenIndex = level
                    WriteOutcome.Applied
                } else {
                    expectedIndex = guarded.appliedIndex
                    WriteOutcome.External(guarded.appliedIndex)
                }
            }
            SystemVolumeWriteResult.Outcome.EXTERNAL -> {
                expectedIndex = guarded.appliedIndex
                WriteOutcome.External(guarded.appliedIndex)
            }
            SystemVolumeWriteResult.Outcome.NOT_LOWER -> WriteOutcome.Refused(guarded.appliedIndex)
            SystemVolumeWriteResult.Outcome.FIXED,
            SystemVolumeWriteResult.Outcome.UNAVAILABLE ->
                WriteOutcome.Unavailable(guarded.message)

            SystemVolumeWriteResult.Outcome.FAILED -> {
                if (guarded.appliedIndex != before.currentIndex) {
                    // The channel moved to a different value while we wrote: this
                    // is not a silent ignore, so keep the user's result.
                    expectedIndex = guarded.appliedIndex
                    WriteOutcome.External(guarded.appliedIndex)
                } else {
                    WriteOutcome.Failed(guarded.message)
                }
            }
        }
    }

    private fun publish(nowMs: Long): SleepTimerStatus {
        val elapsed = (nowMs - startedAtMs).coerceAtLeast(0L)
        status = status.copy(
            phase = phaseAt(elapsed),
            remainingMs = (totalMs - elapsed).coerceAtLeast(0L),
            lastKnownIndex = expectedIndex,
            writtenIndex = writtenIndex,
        )
        return status
    }

    private fun finish(outcome: SleepTimerOutcome, message: String): SleepTimerStatus {
        if (!finished) {
            finished = true
            status = status.copy(
                phase = SleepTimerPhase.FINISHED,
                remainingMs = 0L,
                lastKnownIndex = expectedIndex,
                writtenIndex = writtenIndex,
                outcome = outcome,
                message = message,
                startProblem = null,
            )
        }
        return status
    }

    private fun gateMessage(reason: SleepTimerOutcome): String = when (reason) {
        SleepTimerOutcome.OUTPUT_CHANGED ->
            "检测到音频输出变化，定时已结束；未向新输出写入旧目标音量。"
        SleepTimerOutcome.MANUAL_VOLUME_CHANGE ->
            "检测到手动音量调节，定时已结束并保留当前音量。"
        SleepTimerOutcome.CANCELLED ->
            "已取消睡眠定时；音量保持在当前值，不恢复旧音量。"
        else -> "睡眠定时已结束；音量保持当前值。"
    }

    private fun percentOf(index: Int, snapshot: SystemVolumeSnapshot): Int =
        levelPercent(index, snapshot.minIndex, snapshot.maxIndex)

    private sealed class WriteOutcome {
        data object Applied : WriteOutcome()
        data object Skipped : WriteOutcome()
        data object Aborted : WriteOutcome()
        data class External(val appliedIndex: Int) : WriteOutcome()
        data class Refused(val appliedIndex: Int) : WriteOutcome()
        data class Failed(val message: String?) : WriteOutcome()
        data class Unavailable(val message: String?) : WriteOutcome()
    }
}
