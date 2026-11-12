#include <jni.h>

#include <cstdint>

#include "dreamguard/dsp_core.h"
#include "dreamguard/pcm16_processor.h"

// The Java/Kotlin side lives in com.dreamguard.app.audio.NativeAudioEngine.
// The exported symbol names, the Kotlin method names and the library name
// "dreamguard_dsp" (see CMakeLists.txt) form a contract enforced by
// app/src/test/java/com/dreamguard/app/audio/JniContractTest.kt. They must not
// change, even though nothing here knows about C++ classes any more: all audio
// work goes through the portable C API in dsp-core.
//
// DG-022 adds a second, independent surface:
// com.dreamguard.app.audio.NativePcmProcessor. It owns a dg_pcm16_processor
// (PCM16 -> compressor -> PCM16) and is checked by
// NativePcmProcessorContractTest.kt. The two groups share no symbols or state;
// the soundscape engine keeps its own handle.
namespace {

/// jlong is 64-bit on every Android ABI while the handle is a pointer, so the
/// conversion goes through intptr_t: on armeabi-v7a the pointer is 32-bit and
/// the upper half of the jlong is always zero for a value this bridge produced.
dg_engine* engine_from(jlong handle) {
    return reinterpret_cast<dg_engine*>(static_cast<intptr_t>(handle));
}

jlong handle_to_jlong(const dg_engine* engine) {
    return static_cast<jlong>(reinterpret_cast<intptr_t>(engine));
}

/// Builds the DG-010 configuration from the JNI arguments. Range checking is
/// left to dg_create so there is exactly one source of truth for the limits.
dg_config config_from(jint sampleRate, jint channels) {
    dg_config config{};
    config.sample_rate = static_cast<int32_t>(sampleRate);
    config.channel_count = static_cast<int32_t>(channels);
    return config;
}

// DG-022 PCM16 bridge helpers. A separate handle type from dg_engine: both are
// passed through jlong, but a handle only ever reaches the function group that
// created it.

/// Negative result codes mirrored by NativePcmProcessor.kt. They exist so the
/// Kotlin layer can tell "library/pointer problem, just pass audio through"
/// apart from "the DSP rejected these arguments"; none of them throws.
constexpr jint kPcm16InvalidArgument = -1;
constexpr jint kPcm16NullHandle = -2;
constexpr jint kPcm16NotDirect = -3;
constexpr jint kPcm16DspError = -4;

/// The DSP bypass flag is deliberately NOT exposed here: LocalAudioProcessor's
/// "processing off" path copies the bytes itself, so a bypass never enters JNI.
/// That keeps the bit-exact path free of a native call (and of a compressor
/// handle) and makes this bridge's only job "convert and compress".

dg_pcm16_processor* pcm16_from(jlong handle) {
    return reinterpret_cast<dg_pcm16_processor*>(static_cast<intptr_t>(handle));
}

jlong pcm16_handle_to_jlong(const dg_pcm16_processor* processor) {
    return static_cast<jlong>(reinterpret_cast<intptr_t>(processor));
}

/// Maps a bridge status to the negative JNI code the Kotlin side understands.
jint pcm16_error_code(dg_status status) {
    if (status == DG_ERR_NULL_HANDLE) {
        return kPcm16NullHandle;
    }
    if (status == DG_OK) {
        return 0;
    }
    return kPcm16DspError;
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeCreate(JNIEnv*, jobject,
                                                             jint sampleRate,
                                                             jint channels) {
    const dg_config config = config_from(sampleRate, channels);
    dg_engine* engine = nullptr;
    if (dg_create(&config, &engine) != DG_OK) {
        return 0;
    }
    return handle_to_jlong(engine);
}

JNIEXPORT void JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeDestroy(JNIEnv*, jobject,
                                                             jlong handle) {
    // A zero handle must stay harmless here: dg_destroy reports
    // DG_ERR_NULL_HANDLE and teardown never throws on the Kotlin side. The
    // Kotlin engine clears its handle before destroying it and only destroys
    // after the render thread has returned, so no other call races this one.
    (void)dg_destroy(engine_from(handle));
}

JNIEXPORT void JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeReset(JNIEnv*, jobject,
                                                            jlong handle) {
    (void)dg_reset(engine_from(handle));
}

JNIEXPORT jint JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeRender(JNIEnv* env, jobject,
                                                             jlong handle,
                                                             jfloatArray buffer,
                                                             jint frames) {
    dg_engine* engine = engine_from(handle);
    if (engine == nullptr || buffer == nullptr || frames <= 0) {
        return 0;
    }
    const jsize capacity = env->GetArrayLength(buffer);
    const int32_t channels = dg_channel_count(engine);
    if (channels <= 0) {
        return 0;
    }
    if (capacity < frames * channels) {
        return -1;
    }
    jfloat* samples = env->GetFloatArrayElements(buffer, nullptr);
    if (samples == nullptr) {
        return 0;  // An OutOfMemoryError is already pending.
    }
    uint32_t written = 0;
    const dg_status status = dg_render(
            engine, reinterpret_cast<float*>(samples),
            static_cast<uint32_t>(frames), &written);
    env->ReleaseFloatArrayElements(buffer, samples, 0);
    if (status != DG_OK) {
        return 0;
    }
    return static_cast<jint>(written);
}

JNIEXPORT void JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeSetGain(JNIEnv*, jobject,
                                                              jlong handle,
                                                              jfloat gain) {
    (void)dg_set_gain(engine_from(handle), gain);
}

JNIEXPORT void JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeSetToneCutoff(JNIEnv*,
                                                                    jobject,
                                                                    jlong handle,
                                                                    jfloat hz) {
    (void)dg_set_tone_cutoff_hz(engine_from(handle), hz);
}

JNIEXPORT void JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeSetNoiseMix(JNIEnv*, jobject,
                                                                  jlong handle,
                                                                  jfloat mix) {
    (void)dg_set_noise_mix(engine_from(handle), mix);
}

JNIEXPORT void JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeSetDroneHz(JNIEnv*, jobject,
                                                                 jlong handle,
                                                                 jfloat hz) {
    (void)dg_set_drone_hz(engine_from(handle), hz);
}

JNIEXPORT jstring JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeVersion(JNIEnv* env, jobject) {
    return env->NewStringUTF(dg_version());
}

// ---------------------------------------------------------------------------
// DG-022: PCM16 bridge (NativePcmProcessor)
// ---------------------------------------------------------------------------

JNIEXPORT jlong JNICALL
Java_com_dreamguard_app_audio_NativePcmProcessor_nativeCreate(
        JNIEnv*, jobject, jint sampleRate, jint channelCount, jfloat thresholdDb,
        jfloat ratio, jfloat kneeDb, jfloat attackMs, jfloat releaseMs,
        jfloat makeupGainDb, jboolean linkStereo) {
    dg_pcm16_processor_config config{};
    config.sample_rate = static_cast<int32_t>(sampleRate);
    config.channel_count = static_cast<int32_t>(channelCount);
    config.threshold_db = static_cast<double>(thresholdDb);
    config.ratio = static_cast<double>(ratio);
    config.knee_db = static_cast<double>(kneeDb);
    config.attack_ms = static_cast<double>(attackMs);
    config.release_ms = static_cast<double>(releaseMs);
    config.makeup_gain_db = static_cast<double>(makeupGainDb);
    config.link_stereo = (linkStereo == JNI_TRUE) ? 1 : 0;
    config.limiter_enabled = 1;

    dg_pcm16_processor* processor = nullptr;
    if (dg_pcm16_processor_create(&config, &processor) != DG_OK) {
        return 0;
    }
    return pcm16_handle_to_jlong(processor);
}

JNIEXPORT jint JNICALL
Java_com_dreamguard_app_audio_NativePcmProcessor_nativeProcess(
        JNIEnv* env, jobject, jlong handle, jobject input, jint inputOffset,
        jint inputLength, jobject output, jint outputOffset, jint outputCapacity) {
    dg_pcm16_processor* processor = pcm16_from(handle);
    if (processor == nullptr) {
        return kPcm16NullHandle;
    }
    if (input == nullptr || output == nullptr || inputOffset < 0 || inputLength < 0 ||
        outputOffset < 0 || outputCapacity < 0) {
        return kPcm16InvalidArgument;
    }

    // Direct buffers only: Media3's audio sink hands the processor direct
    // buffers, and this avoids copying into a Java array on every block. A heap
    // buffer is reported as NOT_DIRECT so Kotlin can fall back to its byte copy
    // instead of crashing on a null address.
    const jlong inputBufferCapacity = env->GetDirectBufferCapacity(input);
    const jlong outputBufferCapacity = env->GetDirectBufferCapacity(output);
    if (inputBufferCapacity < 0 || outputBufferCapacity < 0) {
        return kPcm16NotDirect;
    }
    auto* inputBytes = static_cast<std::uint8_t*>(env->GetDirectBufferAddress(input));
    auto* outputBytes = static_cast<std::uint8_t*>(env->GetDirectBufferAddress(output));
    if (inputBytes == nullptr || outputBytes == nullptr) {
        return kPcm16NotDirect;
    }

    if (static_cast<jlong>(inputOffset) + inputLength > inputBufferCapacity ||
        static_cast<jlong>(outputOffset) + inputLength > outputBufferCapacity) {
        return kPcm16InvalidArgument;
    }
    if (outputCapacity < inputLength) {
        return kPcm16InvalidArgument;
    }
    const jlong writable = outputBufferCapacity - static_cast<jlong>(outputOffset);
    const auto capacity = static_cast<uint32_t>(writable < inputLength ? writable : inputLength);

    dg_pcm16_processor_values values{};
    const dg_status status = dg_pcm16_processor_process(
            processor, inputBytes + inputOffset,
            static_cast<uint32_t>(inputLength), outputBytes + outputOffset, capacity,
            0, &values);
    if (status != DG_OK) {
        return pcm16_error_code(status);
    }
    return static_cast<jint>(values.bytes_written);
}

JNIEXPORT jint JNICALL
Java_com_dreamguard_app_audio_NativePcmProcessor_nativeDrain(
        JNIEnv* env, jobject, jlong handle, jobject output, jint outputOffset) {
    auto* processor = pcm16_from(handle);
    if (processor == nullptr) return kPcm16NullHandle;
    if (output == nullptr || outputOffset < 0) return kPcm16InvalidArgument;
    const jlong capacity = env->GetDirectBufferCapacity(output);
    auto* address = static_cast<uint8_t*>(env->GetDirectBufferAddress(output));
    if (capacity < 0 || address == nullptr) return kPcm16NotDirect;
    if (outputOffset > capacity || capacity - outputOffset > UINT32_MAX) return kPcm16InvalidArgument;
    uint32_t bytes = 0;
    const auto status = dg_pcm16_processor_drain(processor, address + outputOffset,
        static_cast<uint32_t>(capacity - outputOffset), &bytes);
    return status == DG_OK ? static_cast<jint>(bytes) : pcm16_error_code(status);
}

JNIEXPORT jint JNICALL
Java_com_dreamguard_app_audio_NativePcmProcessor_nativeReset(JNIEnv*, jobject,
                                                              jlong handle) {
    return pcm16_error_code(dg_pcm16_processor_reset(pcm16_from(handle)));
}

JNIEXPORT jint JNICALL
Java_com_dreamguard_app_audio_NativePcmProcessor_nativeDestroy(JNIEnv*, jobject,
                                                                jlong handle) {
    return pcm16_error_code(dg_pcm16_processor_destroy(pcm16_from(handle)));
}

JNIEXPORT jstring JNICALL
Java_com_dreamguard_app_audio_NativePcmProcessor_nativeVersion(JNIEnv* env, jobject) {
    return env->NewStringUTF(dg_version());
}

}  // extern "C"
