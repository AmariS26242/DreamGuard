# DG-012 reference-comparison harness. Host-only: it writes WAV files and is
# driven by compare_with_ffmpeg.ps1, so it is never part of the Android build.
add_executable(dg_lufs_compare dg_lufs_compare.cpp)

target_link_libraries(dg_lufs_compare PRIVATE dreamguard_dsp_core)

target_compile_features(dg_lufs_compare PRIVATE cxx_std_20)

set_target_properties(dg_lufs_compare PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(dg_lufs_compare PRIVATE /W4)
else()
    target_compile_options(dg_lufs_compare PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Optional in-process reference meter. Off by default because libebur128 is a
# third-party dependency of a verification tool, not of the library. When the
# option is on, both artefacts must be found or the configure step fails loudly
# instead of silently producing a binary without the reference path.
option(DREAMGUARD_LUFS_COMPARE_EBUR128
        "Build dg_lufs_compare with an in-process libebur128 reference meter" OFF)

if(DREAMGUARD_LUFS_COMPARE_EBUR128)
    find_path(EBUR128_INCLUDE_DIR NAMES ebur128.h REQUIRED)
    find_library(EBUR128_LIBRARY NAMES ebur128 REQUIRED)
    target_include_directories(dg_lufs_compare PRIVATE ${EBUR128_INCLUDE_DIR})
    target_link_libraries(dg_lufs_compare PRIVATE ${EBUR128_LIBRARY})
    target_compile_definitions(dg_lufs_compare PRIVATE DREAMGUARD_HAVE_LIBEBUR128=1)
    message(STATUS "dg_lufs_compare: libebur128 reference enabled (${EBUR128_LIBRARY})")
endif()

add_test(NAME dg_lufs_compare COMMAND dg_lufs_compare --selfcheck)
