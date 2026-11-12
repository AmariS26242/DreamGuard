/* DreamGuard DSP core - sample-peak lookahead limiter (DG-014).
 *
 * STATUS: SAMPLE-PEAK TESTED. The sample-peak suite, including the DG-014
 * measurement-only true-peak gap case, passed 2125/0 on an API 35 x86_64
 * emulator. This header still does not describe a true-peak limiter.
 *
 * This is the DG-014 minimum safety limiter. It is deliberately a SAMPLE-PEAK
 * limiter with a finite lookahead: it can guarantee that every output SAMPLE
 * stays at or below the configured ceiling, but it does NOT reconstruct the
 * waveform and therefore does NOT guarantee the true (inter-sample) peak.
 *
 * Sample peak vs true peak
 * ------------------------
 *   - sample peak: max |x[n]| over the stored samples. This is what a normal
 *     digital peak meter shows and what this limiter measures and bounds.
 *   - true peak: the maximum of the reconstructed continuous-time waveform,
 *     which can sit between samples (especially after band-limited content,
 *     lossy coding or sample-rate conversion). Measuring it requires
 *     oversampling (typically at least 4x) or an equivalent reconstruction
 *     filter. This file contains no oversampling and no reconstruction.
 *   Consequences:
 *   - a stream whose samples are all <= ceiling can still produce an
 *     inter-sample peak above it; this limiter will not see that peak and will
 *     not attenuate it;
 *   - "output samples <= ceiling" is therefore NOT a true-peak acceptance
 *     result, and this component must not be described as a true-peak limiter;
 *   - a true-peak claim needs an oversampled measurement of the output (DG-014
 *     roadmap item "oversampling verification"), which is not implemented here.
 *
 * Signal path (per frame)
 * -----------------------
 *   1. required gain: for each channel, req = min(1, ceiling / max(|x|, tiny)).
 *      A non-finite input sample is replaced with 0 and counted; it never
 *      reaches the detector or the output.
 *   2. the required gain enters a sliding minimum over the last
 *      latency_frames + 1 samples (one monotonic deque per channel). The
 *      minimum is the smallest gain that the samples still inside the delay
 *      line need, so the gain currently applied cannot be larger than the
 *      requirement of the sample that is leaving the delay line.
 *   3. envelope: if the window minimum is below the current gain, the gain
 *      drops to it immediately (instant attack, no undershoot); otherwise it
 *      releases toward the window minimum with a one-pole law. It is clamped to
 *      the window minimum every frame, which is what makes the sample bound
 *      hold during release as well.
 *   4. delay: the input is delayed by latency_frames so the gain decision is
 *      made before the sample leaves. Output y[n] = x[n - latency] * gain[n].
 *      The first `latency` output frames are therefore zeros, and the last
 *      `latency` input frames remain inside the delay line until
 *      dg_limiter_flush is called.
 *
 * What this component does NOT do
 * -------------------------------
 *   - no true-peak detection, no oversampling, no reconstruction;
 *   - no click-free guarantee: the instant attack is a step in the gain
 *     envelope. It is safe for the sample bound, but it can produce a click on
 *     a hard transient; DG-013's smoothed compressor is the click-free path;
 *   - no program-dependent release, no auto makeup, no sidechain, no parallel
 *     mix, no dithering;
 *   - no automatic latency compensation for the caller. The caller must delay
 *     the other signal paths by dg_limiter_latency_frames() or flush the tail;
 *   - no claim that a given ceiling "sounds like" a hardware limiter, and no
 *     claim of compliance with any loudness or true-peak standard.
 *
 * Threading and allocation
 * ------------------------
 * create/destroy allocate; process, flush and snapshot do NOT allocate, take no
 * locks and make no syscalls. One handle must be driven by one thread at a
 * time; destroy must not race with process. reset/flush are safe between blocks
 * but not during one.
 *
 * Sample rates: DG_SAMPLE_RATE_MIN..DG_SAMPLE_RATE_MAX (8000..192000 Hz), one or
 * two interleaved channels, the same ranges the rest of the core uses.
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

/* Opaque limiter handle. Never dereference or reinterpret it. */
typedef struct dg_limiter dg_limiter;

typedef struct dg_limiter_config {
    int32_t sample_rate;     /* frames per second, [DG_SAMPLE_RATE_MIN, DG_SAMPLE_RATE_MAX] */
    int32_t channel_count;   /* interleaved channels, [DG_PCM_CHANNELS_MIN, DG_PCM_CHANNELS_MAX] */
    double ceiling_dbfs;     /* [-60, 0]; linear ceiling = 10^(ceiling_dbfs/20) */
    double lookahead_ms;     /* [0, 20]; latency is ceil(lookahead_ms * rate / 1000) frames */
    double release_ms;       /* [1, 5000]; one-pole release time constant */
    int32_t link_stereo;     /* 0 = independent channels, 1 = one shared gain */
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
 * channel count, ceiling, lookahead, release, link flag. */
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

/* Latency in frames introduced by the lookahead, or 0 for NULL. Pure query. */
uint32_t dg_limiter_latency_frames(const dg_limiter* limiter);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DREAMGUARD_DSP_LIMITER_H */
