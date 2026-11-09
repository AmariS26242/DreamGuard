#include <jni.h>

#include <cstdint>
#include <new>
#include <span>

#include "dsp_core.h"

namespace {

using dreamguard::SoundscapeRenderer;

SoundscapeRenderer* renderer_from(jlong handle) {
    return reinterpret_cast<SoundscapeRenderer*>(static_cast<intptr_t>(handle));
}

}  // namespace

// The Java/Kotlin side lives in com.dreamguard.app.audio.NativeAudioEngine.
// Keep these symbol names, the Kotlin method names, and the library name
// "dreamguard_dsp" (see CMakeLists.txt) in sync.
extern "C" {

JNIEXPORT jlong JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeCreate(JNIEnv*, jobject,
                                                             jint sampleRate,
                                                             jint channels) {
    if (sampleRate <= 0 || channels <= 0) {
        return 0;
    }
    auto* renderer = new (std::nothrow) SoundscapeRenderer(sampleRate, channels);
    return reinterpret_cast<jlong>(renderer);
}

JNIEXPORT void JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeDestroy(JNIEnv*, jobject,
                                                              jlong handle) {
    delete renderer_from(handle);
}

JNIEXPORT void JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeReset(JNIEnv*, jobject,
                                                            jlong handle) {
    if (auto* renderer = renderer_from(handle); renderer != nullptr) {
        renderer->reset();
    }
}

JNIEXPORT jint JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeRender(JNIEnv* env, jobject,
                                                             jlong handle,
                                                             jfloatArray buffer,
                                                             jint frames) {
    if (handle == 0 || buffer == nullptr || frames <= 0) {
        return 0;
    }
    auto* renderer = renderer_from(handle);
    if (renderer == nullptr) {
        return 0;
    }
    const jsize capacity = env->GetArrayLength(buffer);
    if (capacity < frames * renderer->channels()) {
        return -1;
    }
    jfloat* samples = env->GetFloatArrayElements(buffer, nullptr);
    if (samples == nullptr) {
        return 0;  // An OutOfMemoryError is already pending.
    }
    const int written = renderer->render(std::span<float>(
        reinterpret_cast<float*>(samples), static_cast<std::size_t>(frames) *
                                                static_cast<std::size_t>(
                                                    renderer->channels())));
    env->ReleaseFloatArrayElements(buffer, samples, 0);
    return written;
}

JNIEXPORT void JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeSetGain(JNIEnv*, jobject,
                                                              jlong handle,
                                                              jfloat gain) {
    if (auto* renderer = renderer_from(handle); renderer != nullptr) {
        renderer->setGain(gain);
    }
}

JNIEXPORT void JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeSetToneCutoff(JNIEnv*,
                                                                    jobject,
                                                                    jlong handle,
                                                                    jfloat hz) {
    if (auto* renderer = renderer_from(handle); renderer != nullptr) {
        renderer->setToneCutoff(hz);
    }
}

JNIEXPORT void JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeSetNoiseMix(JNIEnv*, jobject,
                                                                  jlong handle,
                                                                  jfloat mix) {
    if (auto* renderer = renderer_from(handle); renderer != nullptr) {
        renderer->setNoiseMix(mix);
    }
}

JNIEXPORT void JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeSetDroneHz(JNIEnv*, jobject,
                                                                 jlong handle,
                                                                 jfloat hz) {
    if (auto* renderer = renderer_from(handle); renderer != nullptr) {
        renderer->setDroneHz(hz);
    }
}

JNIEXPORT jstring JNICALL
Java_com_dreamguard_app_audio_NativeAudioEngine_nativeVersion(JNIEnv* env, jobject) {
    return env->NewStringUTF(dreamguard::kDspVersion);
}

}  // extern "C"
