cmake_minimum_required(VERSION 3.22.1)

project(dreamguard_dsp LANGUAGES CXX)

add_library(dreamguard_dsp SHARED
        dsp_core.cpp
        jni_bridge.cpp)

target_compile_features(dreamguard_dsp PRIVATE cxx_std_20)
target_compile_options(dreamguard_dsp PRIVATE
        -Wall
        -Wextra
        -Wpedantic)

target_link_libraries(dreamguard_dsp PRIVATE
        android
        log)

# Matches NativeAudioEngine.LIBRARY_NAME on the Kotlin side.
set_target_properties(dreamguard_dsp PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
