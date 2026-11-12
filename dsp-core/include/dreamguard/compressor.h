/* DreamGuard DSP core - feed-forward dynamic range compressor (DG-013).
 *
 * STATUS: SIGNAL-LEVEL TESTED. The compressor suite passed 348/0 on an
 * API 35 x86_64 emulator. This does not cover listening or physical-device
 * behavior.
 *
 * This is the DG-013 minimum compressor: threshold, ratio, soft knee, attack,
 * release, optional stereo linking and a smoothed gain. It is deliberately a
 * plain feed-forward design with NO lookahead, so it adds no latency and cannot
 * guarantee that an attack transient is caught before it leaves - a peak limiter
 * with lookahead is DG-014, not this file.
 *
 * Signal path (per frame)
 * -----------------------
 *   1. detector: the sample peak |x| of the frame (the maximum over the
 *      channels when linking is on, per channel otherwise). This is a SAMPLE
 *      peak, not a true peak: no oversampling, no reconstruction;
 *   2. level in dBFS: 20*log10(max(peak, DG_COMPRESSOR_LEVEL_FLOOR)), so digital
 *      silence reads -240 dBFS instead of -INFINITY and the curve stays finite;
 *   3. static curve (dB domain, see dg_compressor_static_gain_db): gain reduction
 *      is 0 dB below the knee, (1/ratio - 1) per dB above it, and a quadratic
 *      blend inside the knee. The knee is centred on the threshold;
 *   4. ballistics: the gain reduction is a one-pole follower of the static
 *      target with a separate attack and release time constant. Smoothing the
 *      GAIN (rather than the level detector) is what keeps the gain continuous,
 *      which is why a step input cannot produce a click: the gain moves by at
 *      most (target - current) * (1 - exp(-1 / (tau * fs))) per frame;
 *   5. gain: 10^((gain_reduction_db + makeup_gain_db) / 20), applied to every
 *      channel of the frame. Linked channels share one gain; unlinked channels
 *      have independent detectors and gains.
 *
 * What this component does NOT do
 * -------------------------------
 *   - no true-peak detection, no oversampling, no limiter: with a positive
 *     makeup gain the output CAN exceed 0 dBFS. Overs are counted in
 *     clip_samples and are the caller's business (DG-014);
 *   - no program-dependent ("auto") release, no sidechain input, no parallel
 *     mix, no separate wet/dry;
 *   - no dynamics processing of the sidechain: the detector sees the signal that
 *     is being compressed;
 *   - ratio 1.0 is a legal pass-through (gain is exactly 1.0 when the makeup gain
 *     is 0), which makes "the compressor is off" testable;
 *   - the attack/release constants are defined by the one-pole law above, not by
 *     an analog circuit, and there is no claim that a given millisecond value
 *     sounds like any particular hardware unit.
 *
 * Threading
 * ---------
 * No locks, no atomics, no allocation inside the process path. One handle must
 * be driven by one thread at a time; create/destroy/process/snapshot must be
 * externally serialized. reset() is safe between blocks but not during one.
 *
 * Sample rates: DG_SAMPLE_RATE_MIN..DG_SAMPLE_RATE_MAX (8000..192000 Hz), one or
 * two interleaved channels, the same ranges the rest of the core uses.
 */

#ifndef DREAMGUARD_DSP_COMPRESSOR_H
#define DREAMGUARD_DSP_COMPRESSOR_H

#include <stdint.h>

#include "dreamguard/pcm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parameter ranges. Values outside these are rejected by
 * dg_compressor_create with DG_ERR_INVALID_ARGUMENT. */
#define DG_COMPRESSOR_THRESHOLD_DB_MIN (-100.0)
#define DG_COMPRESSOR_THRESHOLD_DB_MAX (0.0)
#define DG_COMPRESSOR_RATIO_MIN (1.0)
#define DG_COMPRESSOR_RATIO_MAX (100.0)
#define DG_COMPRESSOR_KNEE_DB_MIN (0.0)
#define DG_COMPRESSOR_KNEE_DB_MAX (24.0)
#define DG_COMPRESSOR_ATTACK_MS_MIN (0.1)
#define DG_COMPRESSOR_ATTACK_MS_MAX (500.0)
#define DG_COMPRESSOR_RELEASE_MS_MIN (1.0)
#define DG_COMPRESSOR_RELEASE_MS_MAX (5000.0)
#define DG_COMPRESSOR_MAKEUP_DB_MIN (-24.0)
#define DG_COMPRESSOR_MAKEUP_DB_MAX (24.0)

/* Peak levels below this are clamped before the dB conversion, so silence maps
 * to -240 dBFS and never to -INFINITY. */
#define DG_COMPRESSOR_LEVEL_FLOOR (1e-12)

/* Opaque compressor handle. Never dereference or reinterpret it. */
typedef struct dg_compressor dg_compressor;

typedef struct dg_compressor_config {
    int32_t sample_rate;    /* frames per second, [DG_SAMPLE_RATE_MIN, DG_SAMPLE_RATE_MAX] */
    int32_t channel_count;  /* interleaved channels, [DG_PCM_CHANNELS_MIN, DG_PCM_CHANNELS_MAX] */
    double threshold_db;    /* [DG_COMPRESSOR_THRESHOLD_DB_MIN, DG_COMPRESSOR_THRESHOLD_DB_MAX] */
    double ratio;           /* [1, 100]; 1 = pass-through, 4 = 4:1 */
    double knee_db;         /* [0, 24]; width of the soft knee, centred on the threshold; 0 = hard knee */
    double attack_ms;       /* [0.1, 500]; time constant of the gain-reduction attack */
    double release_ms;      /* [1, 5000]; time constant of the gain-reduction release */
    int32_t link_stereo;    /* 0 = independent channels, 1 = one shared gain */
    double makeup_gain_db;  /* [-24, 24]; applied after compression, not counted in the gain reduction */
} dg_compressor_config;

/* Result of the last processing, written by dg_compressor_snapshot.
 *
 * The peak fields describe the last non-empty dg_compressor_process call;
 * frames and the counters accumulate from create/reset. */
typedef struct dg_compressor_values {
    double gain_reduction_db;        /* current smoothed reduction, <= 0 dB (unlinked: the larger reduction of the two channels) */
    double static_gain_reduction_db; /* the last unsmoothed target the curve produced, <= 0 dB */
    double input_peak_db;            /* peak |x| over the whole last block, 20*log10 with a floor of DG_COMPRESSOR_LEVEL_FLOOR */
    double output_peak_db;           /* peak |y| over the whole last block, same convention; the startup attack IS included, so a single block reports the attack peak, not the settled one */
    uint64_t frames;                 /* frames processed since create/reset */
    uint64_t clip_samples;           /* finite output samples with |y| > 1.0 */
    uint64_t non_finite_samples;     /* NaN/Inf input samples replaced with 0 */
    int32_t linked;                  /* 1 when the channels share one gain */
} dg_compressor_values;

/* Creates a compressor for one stream.
 *
 *   DG_ERR_INVALID_ARGUMENT            config or out_compressor was NULL, or any
 *                                      parameter is outside its documented range
 *   DG_ERR_SAMPLE_RATE_OUT_OF_RANGE    sample_rate outside the documented range
 *   DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE  channel_count outside [1, 2]
 *   DG_ERR_OUT_OF_MEMORY               the handle allocation failed
 *
 * On any failure *out_compressor is set to NULL. Validation order is sample
 * rate, channel count, threshold, ratio, knee, attack, release, makeup gain,
 * link flag. */
dg_status dg_compressor_create(const dg_compressor_config* config,
                               dg_compressor** out_compressor);

/* Releases a handle obtained from dg_compressor_create exactly once.
 *   DG_OK              released
 *   DG_ERR_NULL_HANDLE compressor was NULL */
dg_status dg_compressor_destroy(dg_compressor* compressor);

/* Returns the gain to zero dB and clears every counter and the block peaks.
 * The configuration survives. */
dg_status dg_compressor_reset(dg_compressor* compressor);

/* Compresses frame_count interleaved frames from input to output.
 *
 * The input view and the output buffer must describe the same channel count the
 * compressor was created with, and they must agree with each other on
 * frame_count. frame_count == 0 is a no-op and accepts any channel_count (the
 * other DG-012/DG-013 processors behave the same way).
 *
 * The output is a dg_pcm_buffer (a writable Float32 block), matching
 * dg_limiter_process and the other DSP entry points that write samples. Passing a
 * read-only view as the output is therefore a compile-time error instead of a
 * const_cast hidden inside the implementation.
 *
 * Aliasing contract (same as dg_pcm_apply_gain):
 *   - output.samples == input.samples with the same frame_count/channel_count is
 *     an in-place operation and is supported;
 *   - any other overlap is rejected with DG_ERR_INVALID_ARGUMENT and nothing is
 *     written, because processing interleaved channels through overlapping
 *     buffers is undefined.
 *
 *   DG_OK              frames were processed
 *   DG_ERR_NULL_HANDLE compressor was NULL
 *   DG_ERR_INVALID_ARGUMENT a samples pointer was NULL while frame_count > 0,
 *                     the channel_count is not the configured channel count,
 *                     the descriptors disagree on frame_count, or the buffers
 *                     partially overlap */
dg_status dg_compressor_process(dg_compressor* compressor, dg_pcm_view input,
                                dg_pcm_buffer output);

/* Writes the current values (see dg_compressor_values). O(1).
 *   DG_OK                   *out_values was written
 *   DG_ERR_INVALID_ARGUMENT out_values was NULL
 *   DG_ERR_NULL_HANDLE      compressor was NULL */
dg_status dg_compressor_snapshot(const dg_compressor* compressor,
                                 dg_compressor_values* out_values);

/* Pure query for the static (zero-attack) curve this component uses, without
 * creating a handle. level_db is the detector level in dBFS; *out_gain_db
 * receives the gain REDUCTION in dB, which is <= 0 (0 dB = no compression).
 *
 * The curve is piecewise, with x = level_db - threshold_db:
 *   x <= -knee/2      : 0
 *   x >=  knee/2      : (1/ratio - 1) * x
 *   otherwise         : (1/ratio - 1) * (x + knee/2)^2 / (2*knee)
 *
 *   DG_OK                   *out_gain_db was written
 *   DG_ERR_INVALID_ARGUMENT config or out_gain_db was NULL, a parameter is out
 *                           of range, or level_db is not finite */
dg_status dg_compressor_static_gain_db(const dg_compressor_config* config,
                                       double level_db, double* out_gain_db);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DREAMGUARD_DSP_COMPRESSOR_H */
