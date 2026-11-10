package com.dreamguard.app.audio

import java.util.Locale
import kotlin.math.max

/**
 * DG-025 sleep timer: pure Kotlin model.
 *
 * Everything in this file is Android-free so the countdown rules, validation and
 * formatting run in host JVM tests. The Android service only drives this model on
 * its serial timer thread and moves the resulting state to the notification/UI.
 *
 * Safety rules encoded here:
 *  - the timer only ever lowers the shared `STREAM_MUSIC` index (never raises it);
 *  - the target is clamped to the platform's own minimum index;
 *  - a hold segment performs no volume write at all;
 *  - a cancelled/failed/interrupted/output-changed timer never restores the old
 *    level (there is deliberately no restore path anywhere in this file).
 */

/** How a sleep timer run ended. Every value is a visible terminal state. */
enum class SleepTimerOutcome {
    COMPLETED,
    CANCELLED,
    MANUAL_VOLUME_CHANGE,
    OUTPUT_CHANGED,
    READ_FAILED,
    WRITE_FAILED,
    VOLUME_UNAVAILABLE,
    INTERRUPTED,
    START_REJECTED,
    SERVICE_FAILED,
}

enum class SleepTimerPhase { IDLE, HOLDING, FADING, FINISHED }

/** Why a start request could not be accepted. */
enum class SleepTimerStartProblem {
    READ_FAILED,
    FIXED_VOLUME,
    SINGLE_LEVEL,
    CURRENT_AT_MINIMUM,
    TARGET_NOT_LOWER,
    INVALID_DURATION,
    ALREADY_RUNNING,
}

/**
 * User-configured plan.
 *
 * [fadeDurationMs] is the tail of [totalDurationMs] over which the volume steps
 * down; `0` means "hold for the whole duration, then drop directly at the end".
 * [targetIndex] is a platform stream index, not a percentage.
 */
data class SleepTimerConfig(
    val totalDurationMs: Long,
    val fadeDurationMs: Long,
    val targetIndex: Int,
)

/** Immutable state published by [SleepTimerEngine] for the service/UI. */
data class SleepTimerStatus(
    val phase: SleepTimerPhase = SleepTimerPhase.IDLE,
    /** Service-assigned run token; 0 when no run exists. */
    val session: Long = 0L,
    val config: SleepTimerConfig? = null,
    val startedAtMs: Long = 0L,
    val totalDurationMs: Long = 0L,
    val holdDurationMs: Long = 0L,
    val fadeDurationMs: Long = 0L,
    val remainingMs: Long = 0L,
    val startIndex: Int = 0,
    val lastKnownIndex: Int = 0,
    val targetIndex: Int = 0,
    val minIndex: Int = 0,
    val maxIndex: Int = 0,
    val writtenIndex: Int? = null,
    val outcome: SleepTimerOutcome? = null,
    val message: String? = null,
    val startProblem: SleepTimerStartProblem? = null,
) {
    val isRunning: Boolean
        get() = phase == SleepTimerPhase.HOLDING || phase == SleepTimerPhase.FADING

    val targetPercent: Int
        get() = levelPercent(targetIndex, minIndex, maxIndex)

    val currentPercent: Int
        get() = levelPercent(lastKnownIndex, minIndex, maxIndex)
}

/** Validation shared by the UI (inline reason) and the engine (authoritative check). */
object SleepTimerPolicy {
    const val MIN_TOTAL_DURATION_MS: Long = 10_000L
    const val MAX_TOTAL_DURATION_MS: Long = 120L * 60_000L

    /**
     * Returns null when [config] can be started against [snapshot].
     *
     * A target below the platform minimum is **clamped**, not rejected: the
     * platform's minimum is the lowest level that can exist, and
     * [clampTarget] is what the engine actually uses.
     */
    fun validate(config: SleepTimerConfig, snapshot: SystemVolumeSnapshot): SleepTimerStartProblem? {
        if (!snapshot.isAvailable) return SleepTimerStartProblem.READ_FAILED
        if (snapshot.isFixed) return SleepTimerStartProblem.FIXED_VOLUME
        if (snapshot.maxIndex <= snapshot.minIndex) return SleepTimerStartProblem.SINGLE_LEVEL
        if (config.totalDurationMs < MIN_TOTAL_DURATION_MS ||
            config.totalDurationMs > MAX_TOTAL_DURATION_MS ||
            config.fadeDurationMs < 0L ||
            config.fadeDurationMs > config.totalDurationMs
        ) {
            return SleepTimerStartProblem.INVALID_DURATION
        }
        if (snapshot.currentIndex <= snapshot.minIndex) {
            return SleepTimerStartProblem.CURRENT_AT_MINIMUM
        }
        if (clampTarget(config.targetIndex, snapshot) >= snapshot.currentIndex) {
            return SleepTimerStartProblem.TARGET_NOT_LOWER
        }
        return null
    }

    fun clampTarget(targetIndex: Int, snapshot: SystemVolumeSnapshot): Int =
        targetIndex.coerceIn(snapshot.minIndex, snapshot.maxIndex)

    fun startProblemMessage(problem: SleepTimerStartProblem): String = when (problem) {
        SleepTimerStartProblem.READ_FAILED ->
            "无法读取系统媒体音量，定时未启动。"
        SleepTimerStartProblem.FIXED_VOLUME ->
            "此设备的媒体音量由硬件固定，无法用定时降低，定时未启动。"
        SleepTimerStartProblem.SINGLE_LEVEL ->
            "当前输出只有一个音量级别，无法渐弱，定时未启动。"
        SleepTimerStartProblem.CURRENT_AT_MINIMUM ->
            "系统媒体音量已在最小级别，无法再降低。"
        SleepTimerStartProblem.TARGET_NOT_LOWER ->
            "目标音量必须低于当前系统媒体音量。"
        SleepTimerStartProblem.INVALID_DURATION ->
            "定时参数无效：总时长需在 10 秒至 120 分钟之间，且渐弱时长不能超过总时长。"
        SleepTimerStartProblem.ALREADY_RUNNING ->
            "睡眠定时已在运行。"
    }
}

/** Percent within the platform's own index range (same convention as DG-024). */
fun levelPercent(index: Int, minIndex: Int, maxIndex: Int): Int {
    if (maxIndex <= minIndex) return 0
    val clamped = index.coerceIn(minIndex, maxIndex)
    return Math.round((clamped - minIndex).toFloat() / (maxIndex - minIndex).toFloat() * 100f)
}

/**
 * `mm:ss` remaining label. Rounds up so a running timer never shows `00:00`
 * while it still has work left.
 */
fun formatRemaining(remainingMs: Long): String {
    val seconds = (max(0L, remainingMs) + 999L) / 1000L
    return String.format(Locale.US, "%02d:%02d", seconds / 60L, seconds % 60L)
}
