package com.dreamguard.app.audio

import android.content.Context
import androidx.media3.common.audio.AudioProcessor
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.DefaultRenderersFactory
import androidx.media3.exoplayer.audio.AudioSink
import androidx.media3.exoplayer.audio.DefaultAudioSink

/**
 * Installs [LocalAudioProcessor] into the ExoPlayer audio sink used for local files.
 *
 * The processor is created once per service (one player) and passed in, so the
 * UI can toggle it without rebuilding the player. `DefaultAudioSink.Builder`
 * keeps Media3's own default processors (resampling, channel mapping, silence
 * skipping, Sonic) after ours; only the user's PCM is prepended.
 */
@UnstableApi
class LocalAudioRenderersFactory(
    context: Context,
    private val processor: LocalAudioProcessor,
) : DefaultRenderersFactory(context) {

    override fun buildAudioSink(
        context: Context,
        enableFloatOutput: Boolean,
        enableAudioTrackPlaybackParams: Boolean,
    ): AudioSink =
        DefaultAudioSink.Builder(context)
            .setAudioProcessors(arrayOf<AudioProcessor>(processor))
            .setEnableFloatOutput(enableFloatOutput)
            .setEnableAudioTrackPlaybackParams(enableAudioTrackPlaybackParams)
            .build()
}
