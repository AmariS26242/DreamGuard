/* DreamGuard DSP core - native-endian PCM16 dynamics bridge (DG-022).
 *
 * Zero-initialize config. Default is the zero-latency compressor-only path.
 * limiter_enabled=1 adds the DG-014 conservative 4x guard after compression,
 * ceiling -1 dBFS, linked stereo, 100 ms release, 64 frames of latency.
 * Android enables this mode. Process emits one output frame per input frame,
 * initially silence; drain emits the retained tail at EOS/before bypass. The
 * stream is therefore 64 frames longer, 1.333 ms at 48 kHz or 8 ms at 8 kHz.
 * No audio content is omitted at a normal EOS. Reset/seek discards the old tail.
 * Quantization may add less than 0.001 dB at the fixed ceiling.
 *
 * PCM conversion uses s/32768, rounds half away from zero, and saturates.
 * Bypass copies every byte and leaves audio state untouched. Drain before bypass
 * when a delayed stream is pending. Compressor-only processing also preserves
 * partial frames; guard mode rejects partial frames without writing, so the
 * Android caller can pass them through and report a processing error.
 * Observation windows are frozen during bypass and restart on the next active
 * segment after bypass/drain. Drain includes the wet tail in output metering;
 * its final snapshot remains readable until the next active segment/reset.
 *
 * Create/destroy allocate, process/reset/drain/snapshot do not. Access to one
 * handle must be serialized. The Android owner confines it to the playback thread.
 * One/two channels, 8000..192000 Hz. See docs/evidence for actual test results.
 */

#ifndef DREAMGUARD_DSP_PCM16_PROCESSOR_H
#define DREAMGUARD_DSP_PCM16_PROCESSOR_H

#include <stdint.h>

#include "dreamguard/compressor.h"
#include "dreamguard/dsp_core.h"
#include "dreamguard/lufs.h"
#include "dreamguard/pcm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Frames converted per internal chunk. This bounds the scratch array inside the
 * handle (DG_PCM16_CHUNK_FRAMES * DG_PCM_CHANNELS_MAX floats); it is not a limit
 * on the size of a process call. */
#define DG_PCM16_CHUNK_FRAMES 512

/* Bytes per PCM16 sample. */
#define DG_PCM16_BYTES_PER_SAMPLE 2

/* Interleaved mono/stereo PCM16 in the platform's native byte order, plus the
 * DG-013 compressor parameters. The parameter ranges are the compressor's; see
 * dreamguard/compressor.h. link_stereo is 0 or 1 (any other value is rejected
 * by dg_compressor_create). limiter_enabled must be 0 or 1. */
typedef struct dg_pcm16_processor_config {
    int32_t sample_rate;   /* [DG_SAMPLE_RATE_MIN, DG_SAMPLE_RATE_MAX] */
    int32_t channel_count; /* [DG_PCM_CHANNELS_MIN, DG_PCM_CHANNELS_MAX] */
    double threshold_db;
    double ratio;
    double knee_db;
    double attack_ms;
    double release_ms;
    double makeup_gain_db;
    int32_t link_stereo;
    int32_t limiter_enabled; /* 0 = compressor only, 1 = conservative 4x guard */
} dg_pcm16_processor_config;

/* Result of the last dg_pcm16_processor_process call. */
typedef struct dg_pcm16_processor_values {
    uint32_t frames;           /* whole frames converted (bypass: whole frames copied) */
    uint32_t bytes_written;    /* always equal to the input byte count */
    int32_t bypassed;          /* 1 when the call was a verbatim copy */
    double gain_reduction_db;  /* compressor reduction after the call, <= 0 dB (0 in bypass) */
} dg_pcm16_processor_values;

/* Bounded O(1) loudness & dynamics observation snapshot (P4-01).
 * Reads bounded LUFS windows without scanning the 1-hour integrated block
 * store and allocates nothing. */
typedef struct dg_pcm16_processor_observation {
    double input_momentary_lufs;         /* post-compressor, pre-limiter 400 ms LUFS, or -INFINITY */
    double input_short_term_lufs;        /* post-compressor, pre-limiter 3 s LUFS, or -INFINITY */
    double output_momentary_lufs;        /* post-limiter wet output 400 ms LUFS, or -INFINITY */
    double output_short_term_lufs;       /* post-limiter wet output 3 s LUFS, or -INFINITY */
    double compressor_gain_reduction_db; /* current smoothed compressor reduction, <= 0 dB */
    double makeup_gain_db;               /* configured compressor makeup gain in dB */
    double loudness_gain_db;             /* automatic loudness gain in dB (0 dB in P4-01) */
    double limiter_gain_db;              /* current limiter gain in dB, <= 0 dB (0 when disabled) */
    uint64_t completed_hops;             /* completed 100 ms hops in the current observation segment */
    uint64_t limiter_limited_samples;    /* cumulative channel-samples limited since create/reset */
    uint64_t last_call_limited_samples;  /* channel-samples limited in the last non-empty process call */
    uint32_t hop_frames;                 /* frames per 100 ms hop */
    uint32_t pending_frames;             /* frames accumulated toward the next hop */
    int32_t input_momentary_ready;       /* 1 when >= 400 ms (4 hops) of post-compressor signal observed */
    int32_t input_short_term_ready;      /* 1 when >= 3 s (30 hops) of post-compressor signal observed */
    int32_t output_momentary_ready;      /* 1 when >= 400 ms (4 hops) of wet output signal observed */
    int32_t output_short_term_ready;     /* 1 when >= 3 s (30 hops) of wet output signal observed */
    int32_t limiter_enabled;             /* 1 when the 4x true-peak guard is enabled */
    int32_t limiter_active;              /* 1 when guard limited samples in last call or limiter_gain_db < -1e-4 */
    int32_t bypassed;                    /* 1 when the last non-empty process call was bypass */
} dg_pcm16_processor_observation;

/* Opaque bridge handle. Never dereference or reinterpret it. */
typedef struct dg_pcm16_processor dg_pcm16_processor;

/* Creates a bridge and stores it in *out_processor.
 *
 * Validation is delegated to dg_compressor_create, with the channel count
 * checked first so an unsupported layout reports
 * DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE rather than a generic argument error.
 * On any failure *out_processor is NULL. */
dg_status dg_pcm16_processor_create(const dg_pcm16_processor_config* config,
                                    dg_pcm16_processor** out_processor);

/* Releases a handle exactly once.
 *   DG_OK              released
 *   DG_ERR_NULL_HANDLE handle was NULL */
dg_status dg_pcm16_processor_destroy(dg_pcm16_processor* processor);

/* Resets the compressor and guard, discarding any retained tail; the
 * configuration survives and the scratch buffer is not touched.
 *   DG_OK              reset
 *   DG_ERR_NULL_HANDLE handle was NULL */
dg_status dg_pcm16_processor_reset(dg_pcm16_processor* processor);

/* Processes input_bytes from input to output.
 *
 *   input, output      caller-owned byte ranges. input_bytes == 0 is a no-op and
 *                      accepts NULL pointers.
 *   output_capacity    must be >= input_bytes; otherwise DG_ERR_INVALID_ARGUMENT
 *                      is returned and nothing is written.
 *   bypass             non-zero copies bytes verbatim; zero converts and
 *                      compresses.
 *   out_values         optional; may be NULL.
 *
 * Byte preservation: bytes_written == input_bytes always, including a trailing
 * partial frame in compressor-only mode. Guard mode requires whole frames. The identical input/output pointer is allowed (in-place) and
 * produces the same bytes as separate buffers; any other overlap is rejected
 * without writing.
 *
 *   DG_OK                   the block was processed (or copied)
 *   DG_ERR_INVALID_ARGUMENT NULL pointer with input_bytes > 0, a capacity
 *                           smaller than input_bytes, or partially overlapping
 *                           buffers
 *   DG_ERR_NULL_HANDLE      processor was NULL
 *   (a wrapped dg_compressor_status is returned unchanged if its process call
 *   fails; with a valid handle and matching shapes that cannot happen) */
dg_status dg_pcm16_processor_process(dg_pcm16_processor* processor,
                                     const void* input,
                                     uint32_t input_bytes,
                                     void* output,
                                     uint32_t output_capacity,
                                     int32_t bypass,
                                     dg_pcm16_processor_values* out_values);

/* Optional guard: -1 dBFS, linked, 64 frames of latency (0 when disabled).
 * Process still writes input_bytes. At EOS or before bypass, drain the retained
 * tail once. Reset/seek intentionally discards it. No allocation during drain.
 * Capacity must cover latency * channels * 2 bytes; failure preserves state.
 * An empty/already-drained stream writes zero bytes. */
uint32_t dg_pcm16_processor_latency_frames(const dg_pcm16_processor* processor);
dg_status dg_pcm16_processor_drain(dg_pcm16_processor* processor, void* output,
                                   uint32_t output_capacity, uint32_t* out_bytes);

/* Writes the current observation snapshot (O(1), allocation-free).
 *   DG_OK                   *out_observation was written
 *   DG_ERR_INVALID_ARGUMENT out_observation was NULL
 *   DG_ERR_NULL_HANDLE      processor was NULL */
dg_status dg_pcm16_processor_snapshot(
    const dg_pcm16_processor* processor,
    dg_pcm16_processor_observation* out_observation);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DREAMGUARD_DSP_PCM16_PROCESSOR_H */
