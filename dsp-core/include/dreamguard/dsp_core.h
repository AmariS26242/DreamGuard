/* DreamGuard DSP core - portable C API.
 *
 * This header is the boundary between the native DSP implementation and
 * everything that consumes it (the Android JNI bridge today, a desktop build
 * later). It is plain C99: no C++ types leak through it, and C consumers only
 * need this one include.
 *
 * Scope (DG-010): engine lifetime - dg_create / dg_destroy / dg_reset - with
 * distinguishable error codes and validated configuration. The render and
 * parameter entry points expose the *existing* soundscape generator so the
 * Android build keeps its current audio behaviour and JNI ABI; they are a thin
 * C surface over that generator and are NOT a PCM processor. PCM processing is
 * DG-011 and lives in dreamguard/pcm.h - a separate, stateless component that
 * shares no state with the engine below.
 *
 * Lifecycle contract (deliberately small; the library detects no misuse):
 *  - dg_create allocates an engine and transfers ownership to the caller.
 *  - The handle is valid from a successful dg_create until the matching
 *    dg_destroy. It is opaque: do not dereference or reinterpret it.
 *  - Each handle is released exactly once. Calling any function with NULL is
 *    reported as DG_ERR_NULL_HANDLE; calling any function with a handle that
 *    was already destroyed, never came from dg_create, or was corrupted is
 *    undefined behaviour. The library keeps no registry and does NO stale
 *    handle detection, so it cannot and does not turn that mistake into an
 *    error code. (The JNI bridge never does it: the Kotlin engine clears its
 *    handle before destroying it and destroys only after the render thread has
 *    returned.)
 *  - Independent handles share no state, so different engines can be used from
 *    different threads without any synchronization between them.
 *
 * Per-handle threading contract:
 *  - dg_render and dg_reset mutate per-stream state and must not overlap each
 *    other on the same handle. Callers serialize them, normally by using one
 *    audio thread. The Android engine does exactly that: one render thread owns
 *    the handle for its whole lifetime, and reset runs on that thread when it
 *    finishes.
 *  - dg_set_gain / dg_set_tone_cutoff_hz / dg_set_noise_mix / dg_set_drone_hz
 *    only store an atomic parameter value, so they may be called from any
 *    thread, concurrently with dg_render, dg_reset and with each other. A
 *    parameter takes effect at the next block boundary.
 *  - dg_destroy must be externally synchronized with every other call on that
 *    handle: do not destroy while another thread may still be inside dg_render,
 *    dg_reset, a setter or dg_channel_count. This is the caller's job, and it is
 *    why the audio path contains no locks, no reference counts and no syscalls.
 */

#ifndef DREAMGUARD_DSP_CORE_H
#define DREAMGUARD_DSP_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes are stable: JNI and future hosts branch on them. */
typedef enum dg_status {
    DG_OK = 0,
    /* A required pointer argument was NULL, or a size argument was 0. */
    DG_ERR_INVALID_ARGUMENT = -1,
    /* config->sample_rate outside [DG_SAMPLE_RATE_MIN, DG_SAMPLE_RATE_MAX]. */
    DG_ERR_SAMPLE_RATE_OUT_OF_RANGE = -2,
    /* config->channel_count outside [DG_CHANNELS_MIN, DG_CHANNELS_MAX]. */
    DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE = -3,
    /* Engine allocation failed. */
    DG_ERR_OUT_OF_MEMORY = -4,
    /* The handle argument was NULL. */
    DG_ERR_NULL_HANDLE = -5
} dg_status;

/* Supported configuration range. The generator is mono-per-channel and only
 * needs to cover the sample rates Android audio devices actually report. */
#define DG_SAMPLE_RATE_MIN 8000
#define DG_SAMPLE_RATE_MAX 192000
#define DG_CHANNELS_MIN 1
#define DG_CHANNELS_MAX 2

typedef struct dg_config {
    int32_t sample_rate;   /* frames per second, [DG_SAMPLE_RATE_MIN, DG_SAMPLE_RATE_MAX] */
    int32_t channel_count; /* interleaved channels, [DG_CHANNELS_MIN, DG_CHANNELS_MAX] */
} dg_config;

/* Opaque engine handle. Never dereference or reinterpret it. */
typedef struct dg_engine dg_engine;

/* Creates an engine and stores the handle in *out_engine.
 *
 * On any failure *out_engine is set to NULL and a specific code is returned:
 *   DG_ERR_INVALID_ARGUMENT            config or out_engine was NULL
 *   DG_ERR_SAMPLE_RATE_OUT_OF_RANGE    sample_rate outside the documented range
 *   DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE  channel_count outside the documented range
 *   DG_ERR_OUT_OF_MEMORY               allocation failed
 *
 * Validation order is sample rate, then channel count. On success the caller
 * owns the handle and must pass it to dg_destroy exactly once. */
dg_status dg_create(const dg_config* config, dg_engine** out_engine);

/* Releases a handle obtained from dg_create. Must not race with any other call
 * on the same handle; a second destroy of the same handle is undefined
 * behaviour (and is not detected - see the lifecycle contract above).
 *   DG_OK                  the engine was released
 *   DG_ERR_NULL_HANDLE     engine was NULL */
dg_status dg_destroy(dg_engine* engine);

/* Clears all render state (oscillator phases, filter memory, noise seed and the
 * fade-in ramp) so the next dg_render block is bit-identical to the first block
 * of a freshly created engine. Parameter values are preserved. Must not overlap
 * dg_render on the same handle.
 *   DG_OK                  the engine was reset
 *   DG_ERR_NULL_HANDLE     engine was NULL */
dg_status dg_reset(dg_engine* engine);

/* Renders frame_count interleaved frames into interleaved, which must hold at
 * least frame_count * dg_channel_count(engine) floats. Performs no allocation.
 * *frames_written receives the number of frames actually rendered, and is set
 * to 0 whenever an error is returned. Must not overlap dg_reset on the same
 * handle; dg_set_* may run concurrently.
 *   DG_OK                  *frames_written frames were rendered
 *   DG_ERR_INVALID_ARGUMENT    interleaved or frames_written was NULL,
 *                              or frame_count was 0
 *   DG_ERR_NULL_HANDLE     engine was NULL */
dg_status dg_render(dg_engine* engine,
                    float* interleaved,
                    uint32_t frame_count,
                    uint32_t* frames_written);

/* Parameter setters. They keep the long-standing renderer behaviour: a
 * non-finite value falls back to the documented default and an out-of-range
 * value is clamped, so a live handle never fails with an argument error.
 * Safe to call concurrently with dg_render and with each other.
 *   DG_OK                  the value was accepted (possibly clamped)
 *   DG_ERR_NULL_HANDLE     engine was NULL */
dg_status dg_set_gain(dg_engine* engine, float gain);
dg_status dg_set_tone_cutoff_hz(dg_engine* engine, float cutoff_hz);
dg_status dg_set_noise_mix(dg_engine* engine, float mix);
dg_status dg_set_drone_hz(dg_engine* engine, float hz);

/* Version string of the DSP core, never NULL. */
const char* dg_version(void);

/* Number of interleaved channels of an engine, or 0 for NULL (callers use 0 as
 * "no engine"). Does not take part in the concurrency contract: pass a handle
 * that is alive for the duration of the call. */
int32_t dg_channel_count(const dg_engine* engine);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DREAMGUARD_DSP_CORE_H */
