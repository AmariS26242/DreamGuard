/* DreamGuard DSP core - lookahead limiter (DG-014).
 *
 * Zero-initialize config. true_peak_detection=0 preserves sample-peak behavior.
 * Mode 1 uses a conservative 4x reconstruction bound, not just a peak detector:
 * for each phase/window B = sum |h[j] * x[n-j]|. Each contributing sample is
 * constrained to gain <= ceiling/B before it leaves the delay. The triangle
 * inequality then bounds the reconstructed output even with changing gains.
 *
 * The guard uses a 257-tap Blackman-sinc (65 input-frame history). Its weights
 * also dominate the centered 65-tap measurement filter, bounding both finite
 * 4x reconstructions. Latency is max(ceil(lookahead_ms*rate/1000),64) in mode 1,
 * or the requested lookahead in mode 0. The guard can attenuate more than a
 * cancellation-aware limiter, particularly for HF signals. It is not an
 * analog/broadcast compliance certificate. See docs/evidence for independent
 * 16x FFmpeg comparisons, native regressions and the measured CPU cost.
 *
 * Non-finite samples become zero and are counted. Linked stereo shares gain;
 * independent mode keeps separate bounds and envelopes. Instant attack and
 * one-pole release preserve the bounds, but do not guarantee inaudible gain
 * steps. The first latency_frames outputs are zeros. Call flush at EOS to
 * retrieve the full tail; mode 1 completes the detector with zero context.
 * Reset intentionally discards the old tail, as for a seek.
 *
 * Create/destroy allocate. Process/flush/reset/snapshot allocate nothing, take
 * no locks and make no syscalls. Serialize all access to one handle.
 * Supported: 8000..192000 Hz, one/two interleaved Float32 channels.
 */

#ifndef DREAMGUARD_DSP_LIMITER_H
#define DREAMGUARD_DSP_LIMITER_H

#include <stdint.h>

#include "dreamguard/pcm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parameter ranges. Values outside these are rejected by dg_limiter_create
 * with DG_ERR_INVALID_ARGUMENT. 0 dBFS is allowed: the ceiling is then 1.0. */
#define DG_LIMITER_CEILING_DBFS_MIN (-60.0)
#define DG_LIMITER_CEILING_DBFS_MAX (0.0)
#define DG_LIMITER_LOOKAHEAD_MS_MIN (0.0)
#define DG_LIMITER_LOOKAHEAD_MS_MAX (20.0)
#define DG_LIMITER_RELEASE_MS_MIN (1.0)
#define DG_LIMITER_RELEASE_MS_MAX (5000.0)

/* Peak levels below this are clamped before the dB conversion, so silence maps
 * to -240 dBFS and never to -INFINITY. */
#define DG_LIMITER_LEVEL_FLOOR (1e-12)
#define DG_LIMITER_TRUE_PEAK_CONTEXT 64

/* Opaque limiter handle. Never dereference or reinterpret it. */
typedef struct dg_limiter dg_limiter;

typedef struct dg_limiter_config {
    int32_t sample_rate;     /* frames per second, [DG_SAMPLE_RATE_MIN, DG_SAMPLE_RATE_MAX] */
    int32_t channel_count;   /* interleaved channels, [DG_PCM_CHANNELS_MIN, DG_PCM_CHANNELS_MAX] */
    double ceiling_dbfs;     /* [-60, 0]; linear ceiling = 10^(ceiling_dbfs/20) */
    double lookahead_ms;     /* [0,20]; ceil(ms * rate / 1000), at least 64 in true-peak mode */
    double release_ms;       /* [1, 5000]; one-pole release time constant */
    int32_t link_stereo;     /* 0 = independent channels, 1 = one shared gain */
    int32_t true_peak_detection; /* 0 = sample peak, 1 = conservative 4x guard */
} dg_limiter_config;

/* Result of the last processing, written by dg_limiter_snapshot.
 *
 * The peak fields describe the last non-empty process call; frames and the
 * counters accumulate from create/reset. flush does not change the counters or
 * the block peaks, but it does return the envelope and delay line to their
 * initial state. */
typedef struct dg_limiter_values {
    double ceiling_dbfs;          /* configured ceiling */
    double current_gain_db;       /* current gain, <= 0 dB (unlinked: the channel reducing more) */
    double input_peak_db;         /* peak |x| over the whole last block, 20*log10 with DG_LIMITER_LEVEL_FLOOR */
    double output_peak_db;        /* peak |y| over the whole last block, same convention */
    uint64_t frames;              /* input frames processed since create/reset (flush does not add) */
    uint64_t limited_samples;     /* input channel-samples whose required gain was < 1 */
    uint64_t over_ceiling_samples;/* output samples more than 1e-6 above the linear ceiling (float-rounding tolerance) */
    uint64_t non_finite_samples;  /* NaN/Inf input samples replaced with 0 */
    uint32_t latency_frames;      /* configured delay in frames */
    int32_t linked;               /* 1 when the channels share one gain */
    int32_t true_peak_detection;  /* 1 when the optional detector is enabled */
    double input_true_peak_db;    /* true peak of the last block, when enabled */
} dg_limiter_values;

/* Creates a limiter for one stream.
 *
 *   DG_ERR_INVALID_ARGUMENT            config or out_limiter was NULL, or any
 *                                      parameter is outside its documented range
 *   DG_ERR_SAMPLE_RATE_OUT_OF_RANGE    sample_rate outside the documented range
 *   DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE  channel_count outside [1, 2]
 *   DG_ERR_OUT_OF_MEMORY               the handle or its delay/window buffers
 *                                      could not be allocated
 *
 * On any failure *out_limiter is set to NULL. Validation order is sample rate,
 * channel count, ceiling, lookahead, release, link flag, true-peak flag. */
dg_status dg_limiter_create(const dg_limiter_config* config,
                            dg_limiter** out_limiter);

/* Releases a handle obtained from dg_limiter_create exactly once.
 *   DG_OK            released
 *   DG_ERR_NULL_HANDLE limiter was NULL */
dg_status dg_limiter_destroy(dg_limiter* limiter);

/* Returns the envelope to 1.0 and clears the delay line, the sliding window,
 * every counter and the block peaks. The configuration survives. */
dg_status dg_limiter_reset(dg_limiter* limiter);

/* Limits frame_count interleaved frames from input to output.
 *
 * The input view and the output buffer must describe the same channel count the
 * limiter was created with, and they must agree with each other on frame_count.
 * frame_count == 0 is a no-op and accepts any channel_count (the other DSP
 * processors behave the same way).
 *
 * The output is delayed by dg_limiter_latency_frames(): the first `latency`
 * frames written are zeros, and the final `latency` input frames are retained
 * for dg_limiter_flush. Calling process again continues the same stream; the
 * retained tail is not lost until reset/flush.
 *
 * Aliasing contract (same as dg_pcm_apply_gain and dg_compressor_process):
 *   - output.samples == input.samples with the same frame_count/channel_count is
 *     an in-place operation and is supported;
 *   - any other overlap is rejected with DG_ERR_INVALID_ARGUMENT and nothing is
 *     written.
 *
 *   DG_OK              frames were processed
 *   DG_ERR_NULL_HANDLE limiter was NULL
 *   DG_ERR_INVALID_ARGUMENT a samples pointer was NULL while frame_count > 0,
 *                     the channel_count is not the configured channel count,
 *                     the descriptors disagree on frame_count, or the buffers
 *                     partially overlap */
dg_status dg_limiter_process(dg_limiter* limiter, dg_pcm_view input,
                             dg_pcm_buffer output);

/* Emits the latency_frames samples still held in the delay line and then
 * returns the envelope/delay/window to their initial state (the counters and
 * the last block peaks are preserved). This is the only way to obtain the tail
 * of the stream; after it, the handle is ready for a new stream.
 *
 * output.channel_count must be the configured channel count,
 * output.frame_count must be >= latency_frames, and output.samples must be
 * non-NULL when latency_frames > 0. Exactly latency_frames frames are written;
 * *out_frames receives that number (0 for a zero-latency limiter).
 *
 *   DG_OK              the tail was written (possibly zero frames)
 *   DG_ERR_NULL_HANDLE limiter was NULL
 *   DG_ERR_INVALID_ARGUMENT out_frames was NULL, the channel count is wrong,
 *                     the buffer is too small, or the sample pointer is NULL
 *                     while latency_frames > 0 */
dg_status dg_limiter_flush(dg_limiter* limiter, dg_pcm_buffer output,
                           uint32_t* out_frames);

/* Writes the current values (see dg_limiter_values). O(1). Does not allocate.
 *   DG_OK                   *out_values was written
 *   DG_ERR_INVALID_ARGUMENT out_values was NULL
 *   DG_ERR_NULL_HANDLE      limiter was NULL */
dg_status dg_limiter_snapshot(const dg_limiter* limiter,
                              dg_limiter_values* out_values);

/* Total latency including reconstruction context, or 0 for NULL. Pure query. */
uint32_t dg_limiter_latency_frames(const dg_limiter* limiter);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DREAMGUARD_DSP_LIMITER_H */
