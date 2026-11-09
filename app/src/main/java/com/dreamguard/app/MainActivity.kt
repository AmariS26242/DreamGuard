package com.dreamguard.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import com.dreamguard.app.ui.DreamGuardScreen
import com.dreamguard.app.ui.theme.DreamGuardTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            DreamGuardTheme {
                DreamGuardScreen()
            }
        }
    }
}
