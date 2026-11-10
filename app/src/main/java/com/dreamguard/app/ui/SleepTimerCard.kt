package com.dreamguard.app.ui

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.os.Build
import android.os.IBinder
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.dreamguard.app.audio.AudioManagerVolumeBackend
import com.dreamguard.app.audio.SleepTimerConfig
import com.dreamguard.app.audio.SleepTimerOutcome
import com.dreamguard.app.audio.SleepTimerPhase
import com.dreamguard.app.audio.SleepTimerPolicy
import com.dreamguard.app.audio.SleepTimerRecovery
import com.dreamguard.app.audio.SleepTimerService
import com.dreamguard.app.audio.SleepTimerStatus
import com.dreamguard.app.audio.SleepTimerStore
import com.dreamguard.app.audio.SystemVolumeController
import com.dreamguard.app.audio.SystemVolumeObserver
import com.dreamguard.app.audio.formatRemaining
import com.dreamguard.app.audio.levelPercent
import kotlin.math.roundToInt

private val TOTAL_MINUTE_OPTIONS = listOf(1, 5, 10, 20, 30, 45, 60, 90)
private val FADE_SECOND_OPTIONS = listOf(0, 15, 30, 60, 120, 300, 600)
private val NON_FAILURE_OUTCOMES = setOf(
    SleepTimerOutcome.COMPLETED,
    SleepTimerOutcome.CANCELLED,
    SleepTimerOutcome.MANUAL_VOLUME_CHANGE,
    SleepTimerOutcome.OUTPUT_CHANGED,
)

/**
 * DG-025 sleep-timer card, placed directly below the DG-024 system volume card.
 *
 * Independent of DSP/Media3 playback: starting a timer does not start DreamGuard
 * audio, does not request audio focus, and only lowers the shared `STREAM_MUSIC`
 * channel. The running state lives in [SleepTimerService], so Activity recreation
 * re-binds instead of restarting the countdown.
 */
@Composable
fun SleepTimerCard() {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val controller = remember(context) {
        SystemVolumeController(AudioManagerVolumeBackend(context.applicationContext))
    }
    val store = remember(context) { SleepTimerStore(context.applicationContext) }
    val recovery = remember(store) { SleepTimerRecovery(store) }

    var snapshot by remember(controller) { mutableStateOf(controller.read()) }
    var timerService by remember { mutableStateOf<SleepTimerService?>(null) }
    var revision by remember { mutableStateOf(0) }
    var totalMinutes by remember { mutableStateOf(30) }
    var fadeSeconds by remember { mutableStateOf(60) }
    var targetIndexState by remember { mutableStateOf<Int?>(null) }
    var localError by remember { mutableStateOf<String?>(null) }
    var recovered by remember(recovery) { mutableStateOf(recovery.lastTerminal()) }
    var notificationsDenied by remember { mutableStateOf(false) }
    var permissionRequested by remember { mutableStateOf(false) }

    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted ->
        notificationsDenied = !granted
    }

    DisposableEffect(context) {
        val connection = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName, binder: IBinder) {
                val service = (binder as SleepTimerService.LocalBinder).service
                timerService = service
                service.listener = { revision++ }
                revision++
            }

            override fun onServiceDisconnected(name: ComponentName) {
                timerService = null
                revision++
            }
        }
        val intent = Intent(context, SleepTimerService::class.java)
            .setAction(SleepTimerService.ACTION_BIND_UI)
        val bound = runCatching {
            context.bindService(intent, connection, Context.BIND_AUTO_CREATE)
        }.getOrDefault(false)
        onDispose {
            timerService?.listener = null
            if (bound) runCatching { context.unbindService(connection) }
        }
    }

    DisposableEffect(controller, lifecycleOwner) {
        val observer = SystemVolumeObserver(context.applicationContext) {
            controller.refresh()
        }
        controller.listener = { snapshot = it }
        val lifecycleObserver = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_START -> {
                    observer.start()
                    controller.refresh()
                }
                Lifecycle.Event.ON_STOP -> observer.stop()
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(lifecycleObserver)
        if (lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) {
            observer.start()
            controller.refresh()
        }
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(lifecycleObserver)
            observer.stop()
            controller.listener = null
        }
    }

    val status = remember(timerService, revision) {
        timerService?.currentStatus() ?: SleepTimerStatus()
    }

    val availableFadeSeconds = FADE_SECOND_OPTIONS.filter { it <= totalMinutes * 60 }
    val effectiveFadeSeconds = availableFadeSeconds.firstOrNull { it == fadeSeconds }
        ?: availableFadeSeconds.last()
    val canLower = snapshot.isAvailable &&
        !snapshot.isFixed &&
        snapshot.maxIndex > snapshot.minIndex &&
        snapshot.currentIndex > snapshot.minIndex
    val highestTarget = (snapshot.currentIndex - 1).coerceAtLeast(snapshot.minIndex)
    val target = (targetIndexState ?: highestTarget).coerceIn(snapshot.minIndex, highestTarget)
    val totalMs = totalMinutes.toLong() * 60_000L
    val fadeMs = effectiveFadeSeconds.toLong() * 1_000L
    val config = SleepTimerConfig(totalDurationMs = totalMs, fadeDurationMs = fadeMs, targetIndex = target)
    val problem = SleepTimerPolicy.validate(config, snapshot)

    fun launchStart() {
        localError = null
        if (problem != null) {
            localError = SleepTimerPolicy.startProblemMessage(problem)
            return
        }
        // Single start path: always start the service with the configuration.
        // Do NOT request notification permission before this call: a permission
        // dialog can move the Activity out of the foreground and make a
        // background service start fail. Permission is requested after the run
        // reports RUNNING (see LaunchedEffect below).
        val intent = Intent(context, SleepTimerService::class.java)
            .setAction(SleepTimerService.ACTION_START)
            .putExtra(SleepTimerService.EXTRA_TOTAL_MS, config.totalDurationMs)
            .putExtra(SleepTimerService.EXTRA_FADE_MS, config.fadeDurationMs)
            .putExtra(SleepTimerService.EXTRA_TARGET_INDEX, config.targetIndex)
        val started = runCatching { context.startService(intent) }.isSuccess
        if (!started) {
            localError = "无法启动定时服务（系统限制后台服务），定时未启动。"
            return
        }
        recovery.clearTerminal()
        recovered = null
    }

    fun cancelRunning() {
        localError = null
        val session = status.session
        val service = timerService
        if (service != null) {
            service.cancelTimer(session)
        } else {
            // Stale/alternate path: carry the session token so an old action can
            // never cancel a newer run.
            runCatching {
                context.startService(
                    Intent(context, SleepTimerService::class.java)
                        .setAction(SleepTimerService.ACTION_CANCEL)
                        .putExtra(SleepTimerService.EXTRA_SESSION, session),
                )
            }.onFailure {
                localError = "无法连接定时服务，取消可能未生效。"
            }
        }
    }

    // Request notification permission only after the timer is already running.
    // This keeps the visible start away from the permission dialog race.
    LaunchedEffect(status.isRunning) {
        if (!status.isRunning || permissionRequested) return@LaunchedEffect
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return@LaunchedEffect
        if (ContextCompat.checkSelfPermission(
                context,
                Manifest.permission.POST_NOTIFICATIONS,
            ) == PackageManager.PERMISSION_GRANTED
        ) {
            return@LaunchedEffect
        }
        permissionRequested = true
        permissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
    }

    val notice = when {
        status.isRunning -> null
        status.message != null -> status.message
        recovered != null -> recovered?.message
        else -> null
    }
    val noticeIsFailure = status.startProblem != null ||
        (status.outcome?.let { it !in NON_FAILURE_OUTCOMES } ?: false)
    val recoveredIsFailure = recovered?.outcome?.let { it !in NON_FAILURE_OUTCOMES } ?: false

    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(22.dp),
        color = Color.White.copy(alpha = 0.055f),
    ) {
        Column(modifier = Modifier.padding(horizontal = 18.dp, vertical = 14.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = "睡眠定时",
                        color = MaterialTheme.colorScheme.onSurface,
                        fontSize = 14.sp,
                    )
                    Text(
                        text = "到点后逐级降低系统媒体音量，可随时取消",
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.45f),
                        fontSize = 11.sp,
                    )
                }
                Text(
                    text = when {
                        status.isRunning -> "运行中"
                        status.outcome != null -> "已结束"
                        else -> "未启动"
                    },
                    color = MaterialTheme.colorScheme.primary,
                    fontSize = 13.sp,
                )
            }

            if (status.isRunning) {
                Spacer(Modifier.height(12.dp))
                Text(
                    text = formatRemaining(status.remainingMs),
                    color = MaterialTheme.colorScheme.onSurface,
                    fontSize = 30.sp,
                    fontWeight = FontWeight.Light,
                )
                Text(
                    text = when (status.phase) {
                        SleepTimerPhase.HOLDING -> "保持中 · 此阶段不写入音量"
                        SleepTimerPhase.FADING -> "渐弱中 · 按系统音量级别逐级下调"
                        else -> "运行中"
                    },
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f),
                    fontSize = 12.sp,
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    text = "当前 ${status.currentPercent}% · 目标 ${status.targetPercent}% · " +
                        "渐弱 ${status.fadeDurationMs / 1_000L} 秒",
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f),
                    fontSize = 11.sp,
                )
                Spacer(Modifier.height(12.dp))
                OutlinedButton(
                    onClick = { cancelRunning() },
                    modifier = Modifier.fillMaxWidth(),
                    shape = RoundedCornerShape(16.dp),
                    colors = ButtonDefaults.outlinedButtonColors(
                        contentColor = MaterialTheme.colorScheme.secondary,
                    ),
                ) {
                    Text("取消睡眠定时", fontSize = 14.sp)
                }
            } else {
                Spacer(Modifier.height(12.dp))
                Text(
                    text = "总时长",
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.65f),
                    fontSize = 12.sp,
                )
                Spacer(Modifier.height(6.dp))
                ChoiceRow(
                    options = TOTAL_MINUTE_OPTIONS,
                    selected = totalMinutes,
                    label = { "${it} 分钟" },
                    onSelect = { minutes ->
                        totalMinutes = minutes
                        val maxFade = minutes * 60
                        if (fadeSeconds > maxFade) fadeSeconds = maxFade
                    },
                )

                Spacer(Modifier.height(10.dp))
                Text(
                    text = "渐弱时长",
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.65f),
                    fontSize = 12.sp,
                )
                Spacer(Modifier.height(6.dp))
                ChoiceRow(
                    options = availableFadeSeconds,
                    selected = effectiveFadeSeconds,
                    label = { seconds ->
                        when {
                            seconds == 0 -> "到点直降"
                            seconds < 60 -> "${seconds} 秒"
                            else -> "${seconds / 60} 分钟"
                        }
                    },
                    onSelect = { fadeSeconds = it },
                )

                Spacer(Modifier.height(12.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        text = "目标音量",
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.65f),
                        fontSize = 12.sp,
                    )
                    Text(
                        text = "${levelPercent(target, snapshot.minIndex, snapshot.maxIndex)}%",
                        color = MaterialTheme.colorScheme.primary,
                        fontSize = 12.sp,
                    )
                }
                if (canLower && highestTarget > snapshot.minIndex) {
                    Slider(
                        value = target.toFloat(),
                        onValueChange = { targetIndexState = it.roundToInt() },
                        valueRange = snapshot.minIndex.toFloat()..highestTarget.toFloat(),
                        steps = (highestTarget - snapshot.minIndex - 1).coerceAtLeast(0),
                    )
                } else if (canLower) {
                    Text(
                        text = "目标音量固定为平台最小值 ${snapshot.minIndex}" +
                            "（${levelPercent(snapshot.minIndex, snapshot.minIndex, snapshot.maxIndex)}%）",
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f),
                        fontSize = 11.sp,
                    )
                } else {
                    Text(
                        text = when {
                            !snapshot.isAvailable -> snapshot.readError ?: "无法读取系统媒体音量"
                            snapshot.isFixed -> "媒体音量由硬件固定，无法定时降低"
                            snapshot.maxIndex <= snapshot.minIndex -> "当前输出只有一个音量级别"
                            else -> "系统媒体音量已在最小级别，无法再降低"
                        },
                        color = MaterialTheme.colorScheme.error,
                        fontSize = 11.sp,
                    )
                }

                Spacer(Modifier.height(10.dp))
                OutlinedButton(
                    onClick = { launchStart() },
                    enabled = problem == null,
                    modifier = Modifier.fillMaxWidth(),
                    shape = RoundedCornerShape(16.dp),
                    colors = ButtonDefaults.outlinedButtonColors(
                        contentColor = MaterialTheme.colorScheme.primary,
                    ),
                ) {
                    Text("开始定时", fontSize = 14.sp)
                }
                if (problem != null && canLower) {
                    Spacer(Modifier.height(6.dp))
                    Text(
                        text = SleepTimerPolicy.startProblemMessage(problem),
                        color = MaterialTheme.colorScheme.error,
                        fontSize = 11.sp,
                    )
                }
            }

            localError?.let { message ->
                Spacer(Modifier.height(8.dp))
                Text(
                    text = message,
                    color = MaterialTheme.colorScheme.error,
                    fontSize = 11.sp,
                )
            }

            notice?.let { message ->
                Spacer(Modifier.height(10.dp))
                Text(
                    text = message,
                    color = if (noticeIsFailure || (status.message == null && recoveredIsFailure)) {
                        MaterialTheme.colorScheme.error
                    } else {
                        Color(0xFF9BE7B0)
                    },
                    fontSize = 12.sp,
                )
                if (status.message == null && recovered != null) {
                    TextButton(onClick = {
                        recovery.clearTerminal()
                        recovered = null
                    }) {
                        Text("知道了", fontSize = 12.sp)
                    }
                }
            }

            if (notificationsDenied) {
                Spacer(Modifier.height(8.dp))
                Text(
                    text = "通知权限未授予：系统可能不显示定时通知，但仍可在本卡片取消定时。",
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f),
                    fontSize = 11.sp,
                )
            }

            Spacer(Modifier.height(8.dp))
            Text(
                text = "只降低共享的 STREAM_MUSIC 且不会写入高于当前的值；不启动本应用声音、不申请音频焦点。" +
                    "取消、失败、进程中断或输出切换都不会恢复旧音量。",
                color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.42f),
                fontSize = 11.sp,
            )
        }
    }
}

@Composable
private fun ChoiceRow(
    options: List<Int>,
    selected: Int,
    label: (Int) -> String,
    onSelect: (Int) -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState()),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        options.forEach { option ->
            FilterChip(
                selected = option == selected,
                onClick = { onSelect(option) },
                label = { Text(label(option), fontSize = 12.sp) },
            )
        }
    }
}
