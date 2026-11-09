package com.dreamguard.app.audio

import android.content.Context
import android.net.Uri
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer
import com.dreamguard.app.R

/**
 * Small Media3 surface for the bundled ambient loop. It can be layered on top
 * of the native soundscape or used on its own.
 */
class Media3AmbientPlayer(private val context: Context) {

    private val player: ExoPlayer = ExoPlayer.Builder(context).build().apply {
        setAudioAttributes(
            AudioAttributes.Builder()
                .setUsage(C.USAGE_MEDIA)
                .setContentType(C.AUDIO_CONTENT_TYPE_MUSIC)
                .build(),
            /* handleAudioFocus = */ true,
        )
        repeatMode = Player.REPEAT_MODE_ONE
    }

    var isPlaying: Boolean = false
        private set

    fun toggle(): Boolean {
        if (player.isPlaying) {
            player.pause()
            isPlaying = false
            return false
        }
        if (player.mediaItemCount == 0) {
            val uri = Uri.parse("android.resource://${context.packageName}/${R.raw.dream_loop}")
            player.setMediaItem(MediaItem.fromUri(uri))
            player.prepare()
        }
        player.play()
        isPlaying = true
        return true
    }

    fun pause() {
        if (player.isPlaying) {
            player.pause()
        }
        isPlaying = false
    }

    fun release() {
        isPlaying = false
        player.release()
    }
}
