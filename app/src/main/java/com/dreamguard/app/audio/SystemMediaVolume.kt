package com.dreamguard.app.audio

/**
 * Read/write access to the system media volume channel (`STREAM_MUSIC`).
 *
 * This file deliberately contains no Android type. Range handling, fixed-volume
 * devices, read/write failures and the "state changed externally" rule are all
 * plain Kotlin, so they run in host JVM tests. [AudioManagerVolumeBackend] is the
 * only Android-specific implementation.
 */
interface MediaVolumeBackend {
    /** True when the platform has no software volume (fixed-volume output/policy). */
    fun isVolumeFixed(): Boolean

    /** Lowest valid stream index, inclusive. */
    fun minIndex(): Int

    /** Highest valid stream index, inclusive. */
    fun maxIndex(): Int

    /**
     * Current stream index. May be outside the [minIndex]..[maxIndex] range on a
     * broken OEM audio stack; [SystemVolumeController] clamps it before display.
     */
    fun currentIndex(): Int

    /**
     * Writes one stream index. Must throw [RuntimeException] (for example
     * [SecurityException]) when the platform rejects the write; a silent no-op
     * is detected by the controller re-reading the stream.
     */
    fun setIndex(index: Int)
}

/**
 * Immutable view of the system media channel for one render pass.
 *
 * [isAvailable] is false only when the platform could not be read. A fixed-volume
 * device is *available* (its current level can be shown) but not adjustable.
 */
data class SystemVolumeSnapshot(
    val isAvailable: Boolean,
    val isFixed: Boolean,
    val minIndex: Int,
    val maxIndex: Int,
    val currentIndex: Int,
    val readError: String? = null,
) {
    /** Whether a slider can actually change the level. */
    val isAdjustable: Boolean
        get() = isAvailable && !isFixed && maxIndex > minIndex

    /** Current level as 0..1 over the platform's own index range. */
    val fraction: Float
        get() {
            if (maxIndex <= minIndex) return 0f
            val clamped = currentIndex.coerceIn(minIndex, maxIndex)
            return (clamped - minIndex).toFloat() / (maxIndex - minIndex).toFloat()
        }

    /** Rounded percentage for display; derived from the real range, not hard-coded steps. */
    val percent: Int
        get() = Math.round(fraction * 100f)
}

/** Outcome of one user-driven write, kept separate from the read state. */
data class SystemVolumeWriteResult(
    val outcome: Outcome,
    /** Index the user asked for (never clamped here, so the caller can see the raw value). */
    val requestedIndex: Int,
    /** Index the channel reports after the attempt. */
    val appliedIndex: Int,
    /** User-facing explanation when the write did not take effect. */
    val message: String? = null,
) {
    enum class Outcome {
        /** The platform reports the requested (clamped) level afterwards. */
        APPLIED,

        /** The device has a fixed volume; nothing was written. */
        FIXED,

        /** No usable volume range: read failure or a single-level output. */
        UNAVAILABLE,

        /** The write was rejected, threw, or was silently ignored by the platform. */
        FAILED,

        /**
         * Guarded write only: the channel no longer held the timer's expected
         * index, so nothing was written. The user's/new value wins.
         */
        EXTERNAL,

        /**
         * Guarded write only: the requested level would not lower the channel,
         * so nothing was written.
         */
        NOT_LOWER,
    }

    val isApplied: Boolean get() = outcome == Outcome.APPLIED
}

/**
 * Owns the interaction rules for the system media channel.
 *
 * Threading: create and use it on the main thread only. It performs no work on
 * an audio thread and never requests audio focus, so adjusting it cannot start,
 * pause or duck another app's playback.
 *
 * External changes are polled with [refresh]; the Android observer calls that on
 * every system settings change and the controller publishes only real changes.
 */
class SystemVolumeController(private val backend: MediaVolumeBackend) {

    /**
     * Invoked on the caller's thread when [refresh] observes a state different
     * from the previous published one. It receives the snapshot so the UI does
     * not have to re-read the platform.
     */
    var listener: ((SystemVolumeSnapshot) -> Unit)? = null

    private var lastObserved: SystemVolumeSnapshot? = null

    /** Reads the current state without publishing it through [listener]. */
    fun read(): SystemVolumeSnapshot = readSnapshot()

    /**
     * Re-reads the channel and publishes through [listener] only when something
     * visible changed. Used both after a user write and for hardware-key or
     * other-app changes, so a no-op settings callback cannot churn the UI.
     */
    fun refresh(): SystemVolumeSnapshot {
        val snapshot = readSnapshot()
        if (snapshot != lastObserved) {
            lastObserved = snapshot
            listener?.invoke(snapshot)
        }
        return snapshot
    }

    /**
     * Writes [requestedIndex] clamped into the platform range.
     *
     * Refuses cleanly on fixed-volume devices and when the range cannot be read.
     * After every attempt the channel is re-read, so the returned state is what
     * the system actually holds rather than what was requested.
     */
    fun setIndex(requestedIndex: Int): SystemVolumeWriteResult {
        val before = readSnapshot()
        if (!before.isAvailable) {
            return SystemVolumeWriteResult(
                outcome = SystemVolumeWriteResult.Outcome.UNAVAILABLE,
                requestedIndex = requestedIndex,
                appliedIndex = before.currentIndex,
                message = before.readError ?: "无法读取系统媒体音量",
            )
        }
        if (before.isFixed) {
            return SystemVolumeWriteResult(
                outcome = SystemVolumeWriteResult.Outcome.FIXED,
                requestedIndex = requestedIndex,
                appliedIndex = before.currentIndex,
                message = "此设备的媒体音量由硬件固定，应用无法调节",
            )
        }
        if (before.maxIndex <= before.minIndex) {
            return SystemVolumeWriteResult(
                outcome = SystemVolumeWriteResult.Outcome.UNAVAILABLE,
                requestedIndex = requestedIndex,
                appliedIndex = before.currentIndex,
                message = "当前输出只有一个音量级别，无法调节",
            )
        }

        val target = requestedIndex.coerceIn(before.minIndex, before.maxIndex)
        return try {
            backend.setIndex(target)
            val after = refresh()
            when {
                !after.isAvailable -> SystemVolumeWriteResult(
                    outcome = SystemVolumeWriteResult.Outcome.FAILED,
                    requestedIndex = requestedIndex,
                    appliedIndex = after.currentIndex,
                    message = "已尝试调整，但无法确认系统媒体音量是否生效",
                )
                after.currentIndex == target -> SystemVolumeWriteResult(
                    outcome = SystemVolumeWriteResult.Outcome.APPLIED,
                    requestedIndex = requestedIndex,
                    appliedIndex = after.currentIndex,
                )
                else -> SystemVolumeWriteResult(
                    outcome = SystemVolumeWriteResult.Outcome.FAILED,
                    requestedIndex = requestedIndex,
                    appliedIndex = after.currentIndex,
                    message = "系统未应用这次音量调整（当前 ${after.percent}%）",
                )
            }
        } catch (failure: RuntimeException) {
            // Re-read so the UI shows the value the system still holds, not the
            // one the user dragged to.
            val after = refresh()
            SystemVolumeWriteResult(
                outcome = SystemVolumeWriteResult.Outcome.FAILED,
                requestedIndex = requestedIndex,
                appliedIndex = after.currentIndex,
                message = "调整系统媒体音量失败：${failure.message ?: "系统拒绝"}"
            )
        }
    }

    /**
     * Timer-only guarded write (DG-025).
     *
     * Unlike [setIndex], this re-reads the channel immediately before writing and
     * refuses to write when the current index is not [expectedIndex]. That closes
     * the window in which the timer's earlier tick snapshot is stale and it would
     * otherwise write an old target over a value the user just set. It also
     * refuses to raise the channel or to keep the same level.
     *
     * Normal DG-024 slider writes keep using [setIndex] and its original
     * semantics; [Outcome.EXTERNAL] and [Outcome.NOT_LOWER] are only produced here.
     */
    fun setIndexGuarded(requestedIndex: Int, expectedIndex: Int): SystemVolumeWriteResult {
        val before = readSnapshot()
        if (!before.isAvailable) {
            return SystemVolumeWriteResult(
                outcome = SystemVolumeWriteResult.Outcome.UNAVAILABLE,
                requestedIndex = requestedIndex,
                appliedIndex = before.currentIndex,
                message = before.readError ?: "无法读取系统媒体音量",
            )
        }
        if (before.isFixed) {
            return SystemVolumeWriteResult(
                outcome = SystemVolumeWriteResult.Outcome.FIXED,
                requestedIndex = requestedIndex,
                appliedIndex = before.currentIndex,
                message = "此设备的媒体音量由硬件固定，应用无法调节",
            )
        }
        if (before.maxIndex <= before.minIndex) {
            return SystemVolumeWriteResult(
                outcome = SystemVolumeWriteResult.Outcome.UNAVAILABLE,
                requestedIndex = requestedIndex,
                appliedIndex = before.currentIndex,
                message = "当前输出只有一个音量级别，无法调节",
            )
        }

        // Last system read before the write: the timer's expected index must
        // still be what the channel holds.
        if (before.currentIndex != expectedIndex) {
            return SystemVolumeWriteResult(
                outcome = SystemVolumeWriteResult.Outcome.EXTERNAL,
                requestedIndex = requestedIndex,
                appliedIndex = before.currentIndex,
                message = "检测到系统媒体音量已被外部改变（当前 ${before.percent}%），未写入",
            )
        }

        // Clamp into the platform range, then enforce "only lower".
        val target = requestedIndex.coerceIn(before.minIndex, before.maxIndex)
        if (target >= before.currentIndex) {
            return SystemVolumeWriteResult(
                outcome = SystemVolumeWriteResult.Outcome.NOT_LOWER,
                requestedIndex = requestedIndex,
                appliedIndex = before.currentIndex,
                message = "请求的音量不低于当前值（当前 ${before.percent}%），未写入",
            )
        }

        return try {
            backend.setIndex(target)
            val after = refresh()
            when {
                !after.isAvailable -> SystemVolumeWriteResult(
                    outcome = SystemVolumeWriteResult.Outcome.FAILED,
                    requestedIndex = requestedIndex,
                    appliedIndex = after.currentIndex,
                    message = "已尝试调整，但无法确认系统媒体音量是否生效",
                )
                after.currentIndex == target -> SystemVolumeWriteResult(
                    outcome = SystemVolumeWriteResult.Outcome.APPLIED,
                    requestedIndex = requestedIndex,
                    appliedIndex = after.currentIndex,
                )
                else -> SystemVolumeWriteResult(
                    outcome = SystemVolumeWriteResult.Outcome.FAILED,
                    requestedIndex = requestedIndex,
                    appliedIndex = after.currentIndex,
                    message = "写入后回读不是请求值（当前 ${after.percent}%）",
                )
            }
        } catch (failure: RuntimeException) {
            val after = refresh()
            SystemVolumeWriteResult(
                outcome = SystemVolumeWriteResult.Outcome.FAILED,
                requestedIndex = requestedIndex,
                appliedIndex = after.currentIndex,
                message = "调整系统媒体音量失败：${failure.message ?: "系统拒绝"}",
            )
        }
    }

    private fun readSnapshot(): SystemVolumeSnapshot = try {
        val fixed = backend.isVolumeFixed()
        val min = backend.minIndex()
        val max = backend.maxIndex()
        val current = backend.currentIndex()
        if (min < 0 || max < min) {
            SystemVolumeSnapshot(
                isAvailable = false,
                isFixed = false,
                minIndex = 0,
                maxIndex = 0,
                currentIndex = 0,
                readError = "系统返回了无效的媒体音量范围",
            )
        } else {
            SystemVolumeSnapshot(
                isAvailable = true,
                isFixed = fixed,
                minIndex = min,
                maxIndex = max,
                currentIndex = current.coerceIn(min, max),
            )
        }
    } catch (failure: RuntimeException) {
        SystemVolumeSnapshot(
            isAvailable = false,
            isFixed = false,
            minIndex = 0,
            maxIndex = 0,
            currentIndex = 0,
            readError = "无法读取系统媒体音量：${failure.message ?: "系统限制"}",
        )
    }
}
