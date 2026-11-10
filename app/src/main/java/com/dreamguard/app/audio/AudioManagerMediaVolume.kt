package com.dreamguard.app.audio

import android.content.Context
import android.database.ContentObserver
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings

/**
 * [MediaVolumeBackend] over [AudioManager]'s `STREAM_MUSIC` channel.
 *
 * Permission/behaviour notes, deliberately kept narrow:
 *  - Reading `getStreamVolume` / `getStreamMaxVolume` / `getStreamMinVolume` and
 *    writing `setStreamVolume` on the media stream need no runtime permission and
 *    no `WRITE_SETTINGS`; DreamGuard's manifest is unchanged.
 *  - No audio focus is requested here. Volume is a property of the shared media
 *    channel, so changing it neither starts, pauses nor ducks another app.
 *  - Flags are `0`: the system volume overlay is not requested, and the app's own
 *    card is the only UI feedback.
 *
 * `isVolumeFixed` covers fixed-volume outputs such as some HDMI/TV routes or a
 * device-policy lock; the controller then shows the level read-only instead of
 * pretending a slider can change it.
 */
class AudioManagerVolumeBackend(context: Context) : MediaVolumeBackend {
    // applicationContext: the instance may outlive one Activity composition.
    private val audioManager: AudioManager? =
        context.applicationContext.getSystemService(Context.AUDIO_SERVICE) as? AudioManager

    override fun isVolumeFixed(): Boolean = requireManager().isVolumeFixed

    override fun minIndex(): Int {
        val manager = requireManager()
        // getStreamMinVolume was added in API 28; below that the platform's
        // implicit minimum for the media stream is 0.
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            manager.getStreamMinVolume(AudioManager.STREAM_MUSIC)
        } else {
            0
        }
    }

    override fun maxIndex(): Int =
        requireManager().getStreamMaxVolume(AudioManager.STREAM_MUSIC)

    override fun currentIndex(): Int =
        requireManager().getStreamVolume(AudioManager.STREAM_MUSIC)

    override fun setIndex(index: Int) {
        requireManager().setStreamVolume(AudioManager.STREAM_MUSIC, index, 0)
    }

    private fun requireManager(): AudioManager =
        audioManager ?: throw IllegalStateException("系统音频服务不可用")
}

/**
 * Watches system media-volume changes made outside DreamGuard: hardware volume
 * keys, the system volume panel, or another app.
 *
 * Android's public `AudioManager` volume API has no stream-volume-changed callback
 * on the SDK levels this app targets, so this observes the public
 * `content://settings/system` table. That read needs no permission; every change
 * is re-read through [SystemVolumeController.refresh], which suppresses no-op
 * updates. Registration is tied to the Activity's STARTED state by the caller and
 * is idempotent, so a missed or duplicated lifecycle event cannot leak the observer.
 */
class SystemVolumeObserver(
    context: Context,
    private val onChanged: () -> Unit,
) {
    private val resolver = context.applicationContext.contentResolver
    private val handler = Handler(Looper.getMainLooper())
    private val observer = object : ContentObserver(handler) {
        override fun onChange(selfChange: Boolean) {
            onChanged()
        }
    }

    var isRegistered: Boolean = false
        private set

    fun start() {
        if (isRegistered) return
        // A refused registration only means "no live external updates"; the card
        // still re-reads on ON_START and after every write. Do not crash the UI.
        isRegistered = runCatching {
            resolver.registerContentObserver(Settings.System.CONTENT_URI, true, observer)
            true
        }.getOrDefault(false)
    }

    fun stop() {
        if (!isRegistered) return
        isRegistered = false
        runCatching { resolver.unregisterContentObserver(observer) }
    }
}
