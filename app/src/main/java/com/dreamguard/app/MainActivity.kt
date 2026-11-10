package com.dreamguard.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import com.dreamguard.app.audio.SleepTimerRecovery
import com.dreamguard.app.audio.SleepTimerRuntime
import com.dreamguard.app.audio.SleepTimerStore
import com.dreamguard.app.ui.DreamGuardScreen
import com.dreamguard.app.ui.theme.DreamGuardTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // A previous process may have died while a sleep timer was active. Turn
        // the persisted marker into an explicit terminal prompt exactly once; this
        // performs no volume read-back beyond the store and no volume write.
        SleepTimerRecovery(SleepTimerStore(this)).recoverIfInterrupted(
            liveTimerActive = SleepTimerRuntime.hasLiveService(),
            nowMs = System.currentTimeMillis(),
        )
        enableEdgeToEdge()
        setContent {
            DreamGuardTheme {
                DreamGuardScreen()
            }
        }
    }
}
