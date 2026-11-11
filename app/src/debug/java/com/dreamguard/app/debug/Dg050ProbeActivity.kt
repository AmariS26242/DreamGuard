package com.dreamguard.app.debug

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Typeface
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.os.Process
import android.text.InputType
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import kotlin.math.roundToInt

/**
 * Visible DG-050 entry point (debug only).
 *
 * Nothing starts until the user taps "开始" and then approves both the
 * RECORD_AUDIO runtime permission and a fresh MediaProjection consent. The
 * Activity is exported only in the debug manifest so `adb shell am start -n`
 * can open it, but adb cannot bypass either system dialog.
 *
 * The probe never captures its own UID: it is rejected here and checked again
 * in the non-exported service.
 */
class Dg050ProbeActivity : ComponentActivity() {

    private lateinit var uidInput: EditText
    private lateinit var monitorCheck: CheckBox
    private lateinit var startButton: Button
    private lateinit var stopButton: Button
    private lateinit var statusView: TextView
    private lateinit var eventView: TextView

    private var supported = false
    private var starting = false
    private var cancelPending = false
    private var pendingUid = 0
    private var pendingMonitor = false

    private val events = ArrayList<String>()

    private val busListener: (Dg050ProbeBus.Snapshot) -> Unit = { snapshot ->
        if (::statusView.isInitialized) {
            statusView.text = snapshot.line
            appendEvent(snapshot.line)
            if (snapshot.finished && !snapshot.running) starting = false
            refreshButtons()
        }
    }

    private val recordAudioPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (cancelPending) {
                starting = false
                Dg050ProbeLog.info(
                    "event=permission_result_ignored reason=cancel_requested permission=RECORD_AUDIO",
                )
                refreshButtons()
                return@registerForActivityResult
            }
            if (!granted) {
                starting = false
                Dg050ProbeLog.warn(
                    "event=permission_denied permission=RECORD_AUDIO source_uid=$pendingUid",
                )
                statusView.text = "录音权限被拒绝；未开始捕获。"
                refreshButtons()
                return@registerForActivityResult
            }
            Dg050ProbeLog.info("event=record_audio_granted source_uid=$pendingUid")
            launchProjectionConsent()
        }

    private val projectionConsent =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (cancelPending) {
                starting = false
                Dg050ProbeLog.info(
                    "event=projection_consent_ignored reason=cancel_requested source_uid=$pendingUid",
                )
                refreshButtons()
                return@registerForActivityResult
            }
            val data = result.data
            if (result.resultCode != RESULT_OK || data == null) {
                starting = false
                Dg050ProbeLog.warn(
                    "event=projection_consent_denied result_code=${result.resultCode} " +
                        "source_uid=$pendingUid",
                )
                statusView.text = "屏幕/音频捕获授权被拒绝或取消；未开始捕获。"
                refreshButtons()
                return@registerForActivityResult
            }

            Dg050ProbeLog.info(
                "event=projection_consent_granted source_uid=$pendingUid monitor=$pendingMonitor",
            )
            val intent = Intent(this, Dg050ProbeService::class.java).apply {
                action = Dg050ProbeService.ACTION_START
                putExtra(Dg050ProbeService.EXTRA_PROJECTION_DATA, data)
                putExtra(Dg050ProbeService.EXTRA_RESULT_CODE, result.resultCode)
                putExtra(Dg050ProbeService.EXTRA_SOURCE_UID, pendingUid)
                putExtra(Dg050ProbeService.EXTRA_MONITOR, pendingMonitor)
            }
            try {
                ContextCompat.startForegroundService(this, intent)
                statusView.text = "已请求启动捕获服务；等待 Logcat（tag=${Dg050ProbeLog.TAG}）。"
            } catch (failure: Throwable) {
                starting = false
                Dg050ProbeLog.error(
                    "event=error stage=activity_start_service " +
                        "type=${failure.javaClass.simpleName} message=${sanitize(failure.message)}",
                )
                statusView.text = "捕获服务启动失败：${failure.javaClass.simpleName}"
                refreshButtons()
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        supported = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q
        setContentView(buildUi())

        if (savedInstanceState != null) {
            uidInput.setText(savedInstanceState.getString(STATE_UID_TEXT).orEmpty())
            pendingUid = savedInstanceState.getInt(STATE_PENDING_UID, 0)
            pendingMonitor = savedInstanceState.getBoolean(STATE_PENDING_MONITOR, false)
            cancelPending = savedInstanceState.getBoolean(STATE_CANCEL_PENDING, false)
            monitorCheck.isChecked = pendingMonitor
        } else {
            prefillFromIntent(intent)
        }

        if (!supported) {
            statusView.text =
                "此探针需要 Android 10（API 29）及以上；当前 API ${Build.VERSION.SDK_INT}。"
        } else {
            statusView.text = "空闲：输入来源 UID，点击开始。每次运行都会重新请求授权。"
        }
        refreshButtons()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        if (!Dg050ProbeBus.snapshot.running && !starting) {
            prefillFromIntent(intent)
        }
    }

    override fun onStart() {
        super.onStart()
        if (!supported) return
        Dg050ProbeBus.listener = busListener
        busListener(Dg050ProbeBus.snapshot)
    }

    override fun onStop() {
        if (Dg050ProbeBus.listener === busListener) {
            Dg050ProbeBus.listener = null
        }
        super.onStop()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        outState.putString(STATE_UID_TEXT, uidInput.text.toString())
        outState.putInt(STATE_PENDING_UID, pendingUid)
        outState.putBoolean(STATE_PENDING_MONITOR, pendingMonitor)
        outState.putBoolean(STATE_CANCEL_PENDING, cancelPending)
        super.onSaveInstanceState(outState)
    }

    private fun buildUi(): View {
        val scroll = ScrollView(this).apply { isFillViewport = true }
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(20), dp(20), dp(20))
        }
        scroll.addView(root)

        addBlock(
            root,
            TextView(this).apply {
                text = "DG-050 最小 Debug 捕获探针"
                textSize = 21f
                setTypeface(Typeface.DEFAULT_BOLD)
            },
        )
        addBlock(
            root,
            TextView(this).apply {
                text = "只做 PCM 聚合统计，不保存、不记录 PCM。单次最长 30 秒。\n" +
                    "每次开始都由可见操作触发：RECORD_AUDIO → 新的 MediaProjection 同意 → " +
                    "mediaProjection 前台服务。\n" +
                    "不创建 VirtualDisplay，不使用麦克风，不替换或暂停来源应用。"
                textSize = 13f
            },
            top = 8,
        )

        addBlock(
            root,
            TextView(this).apply {
                text = "来源 UID（必须是正整数；不能是本应用自身 UID）"
                textSize = 13f
            },
            top = 16,
        )
        uidInput = EditText(this).apply {
            hint = "例如 10105"
            inputType = InputType.TYPE_CLASS_NUMBER
            setSingleLine(true)
            textSize = 16f
        }
        addBlock(root, uidInput, top = 4)

        monitorCheck = CheckBox(this).apply {
            isChecked = false
            text = "低增益监视回放（默认关闭；仅应用层 read→write 指标，不代表播放端到端或声学延迟）"
            textSize = 13f
        }
        addBlock(root, monitorCheck, top = 12)

        startButton = Button(this).apply {
            text = "开始（请求录音 + 新的捕获授权）"
            setOnClickListener { startProbe() }
        }
        addBlock(root, startButton, top = 16)

        stopButton = Button(this).apply {
            text = "停止（用户停止）"
            isEnabled = false
            setOnClickListener { stopProbe() }
        }
        addBlock(root, stopButton, top = 8)

        addBlock(
            root,
            TextView(this).apply {
                text = "状态"
                textSize = 13f
                setTypeface(Typeface.DEFAULT_BOLD)
            },
            top = 20,
        )
        statusView = TextView(this).apply {
            text = "空闲。"
            textSize = 14f
            setTextIsSelectable(true)
        }
        addBlock(root, statusView, top = 4)

        addBlock(
            root,
            TextView(this).apply {
                text = "最近事件（同进程快照；完整证据在 Logcat）"
                textSize = 13f
                setTypeface(Typeface.DEFAULT_BOLD)
            },
            top = 16,
        )
        eventView = TextView(this).apply {
            text = "（等待事件）"
            textSize = 11f
            setTypeface(Typeface.MONOSPACE)
            setTextIsSelectable(true)
        }
        addBlock(root, eventView, top = 4)

        addBlock(
            root,
            TextView(this).apply {
                text = "日志：adb logcat -s ${Dg050ProbeLog.TAG}:I\n" +
                    "1 Hz stats：UID、实际格式、frames、nonzero/peak/rms、state、monitor、route、latency。"
                textSize = 11f
                setTypeface(Typeface.MONOSPACE)
            },
            top = 18,
        )
        return scroll
    }

    private fun addBlock(root: LinearLayout, view: View, top: Int = 0) {
        val params = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
        )
        params.topMargin = dp(top)
        root.addView(view, params)
    }

    private fun startProbe() {
        if (!supported) return
        if (starting || Dg050ProbeBus.snapshot.running) {
            statusView.text = "已有会话在运行或启动中。"
            return
        }

        val uid = uidInput.text.toString().trim().toIntOrNull()
        if (uid == null || uid <= 0) {
            Dg050ProbeLog.warn(
                "event=source_uid_rejected reason=invalid_uid " +
                    "source_uid=${sanitize(uidInput.text.toString())}",
            )
            statusView.text = "请输入明确的正整数 UID。"
            return
        }
        if (uid == Process.myUid()) {
            Dg050ProbeLog.warn(
                "event=source_uid_rejected reason=self_uid source_uid=$uid",
            )
            statusView.text = "拒绝：不能以本应用自身 UID（$uid）作为捕获来源。"
            return
        }

        pendingUid = uid
        pendingMonitor = monitorCheck.isChecked
        cancelPending = false
        starting = true
        refreshButtons()
        statusView.text = "准备开始：UID $uid，监视=${if (pendingMonitor) "开" else "关"}。"

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED
        ) {
            Dg050ProbeLog.info("event=record_audio_already_granted source_uid=$uid")
            launchProjectionConsent()
        } else {
            recordAudioPermission.launch(Manifest.permission.RECORD_AUDIO)
        }
    }

    private fun launchProjectionConsent() {
        val manager = getSystemService(MediaProjectionManager::class.java)
        if (manager == null) {
            starting = false
            Dg050ProbeLog.error("event=error stage=projection_manager reason=manager_null")
            statusView.text = "无法获取 MediaProjectionManager。"
            refreshButtons()
            return
        }
        try {
            // One fresh consent per run. The token is used only for audio
            // capture; no VirtualDisplay is ever created from it.
            projectionConsent.launch(manager.createScreenCaptureIntent())
        } catch (failure: Throwable) {
            starting = false
            Dg050ProbeLog.error(
                "event=error stage=consent_launch type=${failure.javaClass.simpleName} " +
                    "message=${sanitize(failure.message)}",
            )
            statusView.text = "无法打开系统捕获授权对话框：${failure.javaClass.simpleName}"
            refreshButtons()
        }
    }

    private fun stopProbe() {
        if (!supported) return
        val running = Dg050ProbeBus.snapshot.running
        if (!starting && !running) return

        // If the consent dialog is still open, the service may not exist yet.
        // Ignore a later consent result and also send an explicit stop so an
        // already-created service cannot keep running unseen.
        cancelPending = !running
        try {
            startService(
                Intent(this, Dg050ProbeService::class.java)
                    .setAction(Dg050ProbeService.ACTION_STOP),
            )
            Dg050ProbeLog.info("event=user_stop_requested source_uid=$pendingUid")
            statusView.text = "已请求停止。"
        } catch (failure: Throwable) {
            Dg050ProbeLog.error(
                "event=error stage=activity_stop_service type=${failure.javaClass.simpleName} " +
                    "message=${sanitize(failure.message)}",
            )
            statusView.text = "停止请求失败：${failure.javaClass.simpleName}"
        }
        starting = false
        refreshButtons()
    }

    private fun refreshButtons() {
        if (!::startButton.isInitialized) return
        val running = Dg050ProbeBus.snapshot.running
        val busy = starting || running
        startButton.isEnabled = supported && !busy
        stopButton.isEnabled = supported && busy
        uidInput.isEnabled = supported && !busy
        monitorCheck.isEnabled = supported && !busy
    }

    private fun prefillFromIntent(intent: Intent?) {
        val source = intent ?: return
        val extras = source.extras ?: return
        val uid = when (val raw = extras.get(Dg050ProbeService.EXTRA_SOURCE_UID)) {
            is Int -> raw
            is String -> raw.toIntOrNull() ?: 0
            else -> 0
        }
        if (uid > 0) uidInput.setText(uid.toString())
        if (source.hasExtra(Dg050ProbeService.EXTRA_MONITOR)) {
            monitorCheck.isChecked = source.getBooleanExtra(Dg050ProbeService.EXTRA_MONITOR, false)
        }
    }

    private fun appendEvent(line: String) {
        if (events.lastOrNull() == line) return
        events.add(line)
        while (events.size > 8) events.removeAt(0)
        eventView.text = events.joinToString("\n")
    }

    private fun sanitize(message: String?): String =
        (message ?: "none").replace(Regex("[\\r\\n\\t ]+"), "_").take(180)

    private fun dp(value: Int): Int =
        (value * resources.displayMetrics.density).roundToInt()

    companion object {
        private const val STATE_UID_TEXT = "dg050_uid_text"
        private const val STATE_PENDING_UID = "dg050_pending_uid"
        private const val STATE_PENDING_MONITOR = "dg050_pending_monitor"
        private const val STATE_CANCEL_PENDING = "dg050_cancel_pending"
    }
}
