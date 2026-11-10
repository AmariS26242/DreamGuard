cmake_minimum_required(VERSION 3.22.1)

project(dreamguard_dsp LANGUAGES CXX)

# The portable DSP core lives in its own Gradle module (settings.gradle.kts ->
# ":dsp-core") with its own CMake project, so the audio generator and the C
# lifecycle API can be built and tested on a host. The Android build pulls the
# very same sources in as a subdirectory: one implementation, one set of tests.
set(DREAMGUARD_DSP_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../dsp-core")
if(NOT EXISTS "${DREAMGUARD_DSP_CORE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
            "DreamGuard DSP core not found at ${DREAMGUARD_DSP_CORE_DIR}")
endif()
add_subdirectory("${DREAMGUARD_DSP_CORE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/dsp-core")

# The JNI bridge is the only Android-specific native code. Its exported symbol
# names and the library name "dreamguard_dsp" are a contract with
# com.dreamguard.app.audio.NativeAudioEngine - do not rename either.
add_library(dreamguard_dsp SHARED
        jni_bridge.cpp)

target_compile_features(dreamguard_dsp PRIVATE cxx_std_20)
target_compile_options(dreamguard_dsp PRIVATE
        -Wall
        -Wextra
        -Wpedantic)

target_link_libraries(dreamguard_dsp PRIVATE
        dreamguard_dsp_core
        android
        log)

# Matches NativeAudioEngine.LIBRARY_NAME on the Kotlin side.
set_target_properties(dreamguard_dsp PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
