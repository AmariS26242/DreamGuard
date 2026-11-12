cmake_minimum_required(VERSION 3.22.1)

project(dreamguard_dsp_core LANGUAGES CXX)

# Portable DSP core: the soundscape generator plus the C lifecycle API from
# DG-010, the stateless Float32 PCM helpers from DG-011, the streaming meter and
# provisional LUFS analysers from DG-012, the DG-013 feed-forward compressor, the
# DG-014 sample-peak lookahead limiter, the DG-014 4x true-peak analyser and the
# DG-022 PCM16 bridge that lets Android playback reach the compressor through JNI.
# It has no Android or platform dependency, so the same sources build for the
# host (tests, desktop tools) and for Android through
# app/src/main/cpp/CMakeLists.txt.
add_library(dreamguard_dsp_core STATIC
        src/compressor.cpp
        src/dg_engine.cpp
        src/limiter.cpp
        src/lufs.cpp
        src/meter.cpp
        src/pcm.cpp
        src/pcm16_processor.cpp
        src/soundscape.cpp
        src/true_peak.cpp)

target_include_directories(dreamguard_dsp_core
        PUBLIC include
        PRIVATE src)

target_compile_features(dreamguard_dsp_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(dreamguard_dsp_core PRIVATE /W4)
else()
    target_compile_options(dreamguard_dsp_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(dreamguard_dsp_core PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)

# Tests are on when this file is the top-level project (host checkout) and off
# when it is pulled into the Android build, which must not build host test
# binaries inside an NDK toolchain.
if(PROJECT_IS_TOP_LEVEL)
    set(DREAMGUARD_DSP_TESTS_DEFAULT ON)
else()
    set(DREAMGUARD_DSP_TESTS_DEFAULT OFF)
endif()

option(DREAMGUARD_DSP_BUILD_TESTS
        "Build the portable DSP core tests (host builds only)"
        ${DREAMGUARD_DSP_TESTS_DEFAULT})

# Host-only verification tools. They write files and shell out to reference
# meters, so they follow the same rule as the tests: on for a top-level host
# checkout, never inside the Android build.
option(DREAMGUARD_DSP_BUILD_TOOLS
        "Build the host-only DSP verification tools (host builds only)"
        ${DREAMGUARD_DSP_TESTS_DEFAULT})

if(DREAMGUARD_DSP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(DREAMGUARD_DSP_BUILD_TOOLS)
    enable_testing()
    add_subdirectory(tools/lufs_compare)
    add_subdirectory(tools/bench)
endif()
