package com.dreamguard.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.IBinder
import com.dreamguard.app.audio.AudioSettings
import com.dreamguard.app.audio.PlaybackService
import java.util.Locale

@Composable
fun DreamGuardScreen() {
    val context = LocalContext.current
    var service by remember { mutableStateOf<PlaybackService?>(null) }
    var revision by remember { mutableStateOf(0) }
    DisposableEffect(context) {
        val connection = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName, binder: IBinder) {
                service = (binder as PlaybackService.LocalBinder).service.also {
                    it.listener = { revision++ }
                }
                revision++
            }
            override fun onServiceDisconnected(name: ComponentName) { service = null }
        }
        val intent = Intent(context, PlaybackService::class.java).setAction(PlaybackService.ACTION_BIND_UI)
        val bound = context.bindService(intent, connection, Context.BIND_AUTO_CREATE)
        onDispose {
            service?.listener = null
            if (bound) context.unbindService(connection)
        }
    }
    val playback = service
    if (playback == null) {
        // The service bind is asynchronous; do not blank the whole screen while it runs.
        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(Color(0xFF0B1026)),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                text = "正在连接播放服务…",
                color = Color(0xFFB6BCDA),
                fontSize = 13.sp,
            )
        }
        return
    }
    @Suppress("UNUSED_VARIABLE") val stateRevision = revision
    val controller = playback.controller
    val snapshot = controller.snapshot()
    val ambientEnabled = playback.ambientEnabled
    val playbackError = playback.error

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(
                Brush.verticalGradient(
                    colors = listOf(
                        Color(0xFF0B1026),
                        Color(0xFF141A3A),
                        Color(0xFF2A1B3D),
                    ),
                ),
            )
            .safeDrawingPadding(),
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 24.dp, vertical = 20.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                text = "DREAMGUARD",
                color = MaterialTheme.colorScheme.primary.copy(alpha = 0.75f),
                fontSize = 12.sp,
                fontWeight = FontWeight.SemiBold,
                letterSpacing = 3.sp,
            )
            Spacer(Modifier.height(18.dp))
            Text(
                text = "沉入睡眠",
                color = MaterialTheme.colorScheme.onBackground,
                fontSize = 34.sp,
                fontWeight = FontWeight.Light,
                letterSpacing = 2.sp,
            )
            Spacer(Modifier.height(8.dp))
            Text(
                text = "C++20 实时声景 · 本地生成 · 无网络",
                color = MaterialTheme.colorScheme.onBackground.copy(alpha = 0.6f),
                fontSize = 13.sp,
                textAlign = TextAlign.Center,
            )

            Spacer(Modifier.height(32.dp))

            PlayButton(
                // A deferred start is still the user's active play session; showing
                // "play" here would make the next tap read as a start when it pauses.
                isRunning = snapshot.isRunning || snapshot.retryableStart,
                enabled = snapshot.isAvailable,
                onClick = { playback.toggleDsp() },
            )

            Spacer(Modifier.height(18.dp))

            val statusText = when {
                !snapshot.isAvailable -> "DSP 不可用（当前 ABI 未打包原生库）"
                // The engine is healthy; it is waiting for the previous render thread
                // to return. Saying so is better than a silent "standby".
                snapshot.retryableStart -> "正在等待上一段音频结束…"
                snapshot.isRunning -> "运行中 · DSP v${playback.version}"
                else -> "待机"
            }
            val statusColor = when {
                !snapshot.isAvailable -> MaterialTheme.colorScheme.error
                snapshot.retryableStart -> MaterialTheme.colorScheme.onBackground.copy(alpha = 0.7f)
                snapshot.isRunning -> Color(0xFF9BE7B0)
                else -> MaterialTheme.colorScheme.onBackground.copy(alpha = 0.55f)
            }
            Text(
                text = statusText,
                color = statusColor,
                fontSize = 13.sp,
            )

            // A deferred start is not an error; showing the old failure text next to
            // "waiting for the previous session" would contradict the status line.
            if (!snapshot.retryableStart) {
                snapshot.lastError?.let { message ->
                    Spacer(Modifier.height(10.dp))
                    Text(
                        text = message,
                        color = MaterialTheme.colorScheme.error,
                        fontSize = 12.sp,
                        textAlign = TextAlign.Center,
                    )
                }

                // Service-level failures (player error, exhausted retries, foreground
                // service refusal) are shown in the same place as engine failures so
                // they cannot be lost below the fold or mistaken for a user pause.
                playbackError?.let { message ->
                    Spacer(Modifier.height(10.dp))
                    Text(
                        text = message,
                        color = MaterialTheme.colorScheme.error,
                        fontSize = 12.sp,
                        textAlign = TextAlign.Center,
                    )
                }
            }

            Spacer(Modifier.height(28.dp))

            ControlCard(
                title = "音量",
                valueLabel = "${(snapshot.settings.gain * 100).toInt()}%",
            ) {
                Slider(
                    value = snapshot.settings.gain,
                    onValueChange = { controller.setGain(it) },
                    valueRange = AudioSettings.MIN_GAIN..AudioSettings.MAX_GAIN,
                )
            }

            Spacer(Modifier.height(12.dp))

            ControlCard(
                title = "音色亮度",
                valueLabel = formatCutoff(snapshot.settings.toneCutoffHz),
            ) {
                Slider(
                    value = snapshot.settings.toneCutoffHz,
                    onValueChange = { controller.setToneCutoff(it) },
                    valueRange = AudioSettings.MIN_TONE_CUTOFF_HZ..AudioSettings.MAX_TONE_CUTOFF_HZ,
                )
            }

            Spacer(Modifier.height(12.dp))

            ControlCard(
                title = "噪声音量",
                valueLabel = "${(snapshot.settings.noiseMix * 100).toInt()}%",
            ) {
                Slider(
                    value = snapshot.settings.noiseMix,
                    onValueChange = { controller.setNoiseMix(it) },
                    valueRange = AudioSettings.MIN_NOISE_MIX..AudioSettings.MAX_NOISE_MIX,
                )
            }

            Spacer(Modifier.height(12.dp))

            ControlCard(
                title = "低频频率",
                valueLabel = "${snapshot.settings.droneHz.toInt()} Hz",
            ) {
                Slider(
                    value = snapshot.settings.droneHz,
                    onValueChange = { controller.setDroneHz(it) },
                    valueRange = AudioSettings.MIN_DRONE_HZ..AudioSettings.MAX_DRONE_HZ,
                )
            }

            Spacer(Modifier.height(22.dp))

            OutlinedButton(
                onClick = { playback.toggleAmbient() },
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(16.dp),
                colors = ButtonDefaults.outlinedButtonColors(
                    contentColor = MaterialTheme.colorScheme.secondary,
                ),
            ) {
                Text(
                    text = if (ambientEnabled) "关闭环境音轨（Media3）" else "播放环境音轨（Media3）",
                    fontSize = 14.sp,
                )
            }

            Spacer(Modifier.height(28.dp))
            Text(
                text = "无网络 / 麦克风 / 存储权限 · 声音只在本机合成",
                color = MaterialTheme.colorScheme.onBackground.copy(alpha = 0.42f),
                fontSize = 11.sp,
                textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(12.dp))
        }
    }
}

@Composable
private fun PlayButton(
    isRunning: Boolean,
    enabled: Boolean,
    onClick: () -> Unit,
) {
    val halo = if (isRunning) {
        Brush.radialGradient(
            colors = listOf(Color(0xFF8FB2FF), Color(0xFF3B4E9B)),
        )
    } else {
        Brush.radialGradient(
            colors = listOf(Color(0xFF3A4270), Color(0xFF1A2044)),
        )
    }
    Box(
        modifier = Modifier
            .size(168.dp)
            .clip(CircleShape)
            .background(halo)
            .clickable(
                enabled = enabled,
                role = Role.Button,
                onClickLabel = if (isRunning) "暂停声景" else "播放声景",
                onClick = onClick,
            ),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = if (isRunning) "Ⅱ" else "▶",
            color = MaterialTheme.colorScheme.onBackground,
            fontSize = 58.sp,
            fontWeight = FontWeight.Light,
        )
    }
}

@Composable
private fun ControlCard(
    title: String,
    valueLabel: String,
    content: @Composable () -> Unit,
) {
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
                Text(
                    text = title,
                    color = MaterialTheme.colorScheme.onSurface,
                    fontSize = 14.sp,
                )
                Text(
                    text = valueLabel,
                    color = MaterialTheme.colorScheme.primary,
                    fontSize = 13.sp,
                )
            }
            content()
        }
    }
}

private fun formatCutoff(hz: Float): String = if (hz >= 1000f) {
    String.format(Locale.US, "%.1f kHz", hz / 1000f)
} else {
    "${hz.toInt()} Hz"
}
