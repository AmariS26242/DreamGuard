# Host tests for the portable DSP core. Registered with CTest; run them with
#   ctest --test-dir build/native --output-on-failure
# or through Gradle with `./gradlew :dsp-core:dspCoreTest`.

enable_language(C)

add_executable(dg_lifecycle_tests test_dg_lifecycle.cpp)
target_link_libraries(dg_lifecycle_tests PRIVATE dreamguard_dsp_core)
target_compile_features(dg_lifecycle_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(dg_lifecycle_tests PRIVATE /W4)
else()
    target_compile_options(dg_lifecycle_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The public headers must stay usable from plain C. This translation unit is
# compiled as C (no `extern "C"` help) and only includes the installed headers;
# it is linked as C++ because the core it talks to is a C++ library.
add_executable(dg_header_c_compat test_header_c_compat.c)
target_link_libraries(dg_header_c_compat PRIVATE dreamguard_dsp_core)
set_target_properties(dg_header_c_compat PROPERTIES LINKER_LANGUAGE CXX)

add_executable(dg_pcm_tests test_pcm.cpp)
target_link_libraries(dg_pcm_tests PRIVATE dreamguard_dsp_core)
target_compile_features(dg_pcm_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(dg_pcm_tests PRIVATE /W4)
else()
    target_compile_options(dg_pcm_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(dg_meter_tests test_meter.cpp)
target_link_libraries(dg_meter_tests PRIVATE dreamguard_dsp_core)
target_compile_features(dg_meter_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(dg_meter_tests PRIVATE /W4)
else()
    target_compile_options(dg_meter_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(dg_lufs_tests test_lufs.cpp)
target_link_libraries(dg_lufs_tests PRIVATE dreamguard_dsp_core)
target_compile_features(dg_lufs_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(dg_lufs_tests PRIVATE /W4)
else()
    target_compile_options(dg_lufs_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(dg_compressor_tests test_compressor.cpp)
target_link_libraries(dg_compressor_tests PRIVATE dreamguard_dsp_core)
target_compile_features(dg_compressor_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(dg_compressor_tests PRIVATE /W4)
else()
    target_compile_options(dg_compressor_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(dg_limiter_tests test_limiter.cpp)
target_link_libraries(dg_limiter_tests PRIVATE dreamguard_dsp_core)
target_compile_features(dg_limiter_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(dg_limiter_tests PRIVATE /W4)
else()
    target_compile_options(dg_limiter_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

# DG-014: 4x oversampled true-peak analyser (measurement only).
add_executable(dg_true_peak_tests test_true_peak.cpp)
target_link_libraries(dg_true_peak_tests PRIVATE dreamguard_dsp_core)
target_compile_features(dg_true_peak_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(dg_true_peak_tests PRIVATE /W4)
else()
    target_compile_options(dg_true_peak_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

# DG-022: PCM16 <-> compressor bridge used by the Android JNI layer.
add_executable(dg_pcm16_processor_tests test_pcm16_processor.cpp)
target_link_libraries(dg_pcm16_processor_tests PRIVATE dreamguard_dsp_core)
target_compile_features(dg_pcm16_processor_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(dg_pcm16_processor_tests PRIVATE /W4)
else()
    target_compile_options(dg_pcm16_processor_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME dg_lifecycle COMMAND dg_lifecycle_tests)
add_test(NAME dg_header_c_compat COMMAND dg_header_c_compat)
add_test(NAME dg_pcm COMMAND dg_pcm_tests)
add_test(NAME dg_meter COMMAND dg_meter_tests)
add_test(NAME dg_lufs COMMAND dg_lufs_tests)
add_test(NAME dg_compressor COMMAND dg_compressor_tests)
add_test(NAME dg_limiter COMMAND dg_limiter_tests)
add_test(NAME dg_true_peak COMMAND dg_true_peak_tests)
add_test(NAME dg_pcm16_processor COMMAND dg_pcm16_processor_tests)
