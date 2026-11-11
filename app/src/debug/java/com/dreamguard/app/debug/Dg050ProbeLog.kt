package com.dreamguard.app.debug

import android.os.Handler
import android.os.Looper
import android.util.Log

/**
 * DG-050 evidence log. One fixed tag, line-oriented `key=value` records, so a
 * short run can be reviewed with:
 *
 *     adb logcat -s DG050Probe:I
 *
 * Aggregate statistics are logged only; no PCM block, file or array is ever
 * written by this probe.
 */
internal object Dg050ProbeLog {
    const val TAG = "DG050Probe"

    fun info(line: String) {
        Log.i(TAG, line)
        Dg050ProbeBus.update(line)
    }

    fun warn(line: String) {
        Log.w(TAG, line)
        Dg050ProbeBus.update(line)
    }

    fun error(line: String) {
        Log.e(TAG, line)
        Dg050ProbeBus.update(line)
    }

    /** 1 Hz aggregate: Logcat only, not pushed into the Activity on every tick. */
    fun stats(line: String) {
        Log.i(TAG, line)
    }
}

/**
 * Same-process snapshot for the debug Activity. The service is non-exported and
 * both components run in the app process; this object is not a persistence
 * mechanism and resets with the process.
 */
internal object Dg050ProbeBus {
    data class Snapshot(
        val running: Boolean,
        val finished: Boolean,
        val line: String,
    )

    @Volatile
    var snapshot: Snapshot = Snapshot(
        running = false,
        finished = false,
        line = "空闲：等待一次可见的开始操作。",
    )
        private set

    private val mainHandler = Handler(Looper.getMainLooper())

    var listener: ((Snapshot) -> Unit)? = null

    fun update(line: String) {
        snapshot = snapshot.copy(line = line)
        mainHandler.post { listener?.invoke(snapshot) }
    }

    fun setRunning(running: Boolean, line: String, finished: Boolean = false) {
        snapshot = Snapshot(running = running, finished = finished, line = line)
        mainHandler.post { listener?.invoke(snapshot) }
    }
}
