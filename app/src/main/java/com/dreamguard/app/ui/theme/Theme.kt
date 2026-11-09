package com.dreamguard.app.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val NightBlue = Color(0xFF0B1026)
private val DeepIndigo = Color(0xFF1B2350)
private val Moonlight = Color(0xFFAFC6FF)
private val SoftViolet = Color(0xFFCBB8FF)
private val Mist = Color(0xFFDCE4FF)
private val Warning = Color(0xFFFFB4AB)

private val DreamGuardColors = darkColorScheme(
    primary = Moonlight,
    onPrimary = NightBlue,
    primaryContainer = DeepIndigo,
    onPrimaryContainer = Mist,
    secondary = SoftViolet,
    onSecondary = NightBlue,
    background = NightBlue,
    onBackground = Mist,
    surface = DeepIndigo,
    onSurface = Mist,
    surfaceVariant = Color(0xFF252D5C),
    onSurfaceVariant = Color(0xFFC2C9EA),
    error = Warning,
)

@Composable
fun DreamGuardTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = DreamGuardColors,
        content = content,
    )
}
