/* DreamGuard DSP core - 4x oversampled true-peak analyser (DG-014).
 *
 * STATUS: Native signal regressions passed on the API 35 x86_64 emulator.
 * Independent limiter-output comparisons are recorded in docs/evidence.
 * Physical-device listening is pending. This analyser itself is measurement only.
 *
 * What this is
 * ------------
 * A stateful, measurement-only analyser for one interleaved Float32 stream. It
 * reconstructs the waveform at 4x the input rate with a windowed-sinc
 * polyphase interpolator and reports the largest magnitude it sees. That
 * magnitude is the true (inter-sample) peak of this 4x reconstruction, not a
 * standard-compliance certificate: it does not implement ITU-R BS.1770-4 /
 * EBU R128 metering, it is not an oversampled limiter, and it does not claim
 * that a stream passing this measurement is compliant with any broadcast
 * specification.
 *
 * What it measures
 * ----------------
 *   sample_peak_*  max |x| over the frames of the most recent non-empty
 *                  process call (the ordinary digital sample peak);
 *   true_peak_*    max |y| over the 4x interpolated samples produced by that
 *                  same call, including the original sample positions;
 *   total_*        the same maxima accumulated since create/reset.
 * The 4x filter has a fixed group delay of DG_TRUE_PEAK_FILTER_LATENCY frames;
 * it is a measurement delay only. The caller does not need to compensate it,
 * because the analyser reports magnitudes, not a delayed signal.
 *
 * Filter and accuracy
 * -------------------
 * The interpolator is a 65-tap linear-phase windowed-sinc low-pass (Blackman
 * window, 16 taps per polyphase branch plus the centre tap). At DC every
 * branch sums to 1, and the phase-0 branch is an exact delayed copy of the
 * input, so the original samples are included in the true-peak maximum. The
 * filter is designed for input rates DG_SAMPLE_RATE_MIN..DG_SAMPLE_RATE_MAX
 * (8 kHz..192 kHz); its stopband and passband ripple are not characterised
 * against a reference implementation in this round. A 4x reconstruction is the
 * minimum the DG-014 roadmap asks for; it is not a proof about the analog
 * waveform.
 *
 * Edge behavior: the reconstruction is causal and needs
 * DG_TRUE_PEAK_FILTER_LATENCY more frames after a sample before that sample
 * reaches the centre tap. A stream that ends immediately after a transient can
 * therefore report a true peak lower than the steady-state reconstruction.
 * There is no flush call in this round; process 16 zero frames if a complete
 * tail is required. The block/total sample peaks are not affected by this
 * delay.
 *
 * Numeric boundaries
 * ------------------
 *   - zero input: peaks are 0 and their dBFS values are -240 dBFS;
 *   - NaN and +/-Inf are not trusted: each is counted in non_finite_samples,
 *     replaced by 0 in the interpolation history, and contributes 0 to every
 *     peak. A snapshot value is never NaN or Inf because of a bad input sample;
 *   - finite samples are used as-is, including magnitudes above 1.0 (this is a
 *     measurement, not a clipper);
 *   - dBFS = 20*log10(linear), with linear values below DG_TRUE_PEAK_LEVEL_FLOOR
 *     clamped before the logarithm so silence maps to a finite -240 dBFS
 *     instead of -INFINITY. This matches the limiter/meter convention used
 *     elsewhere in the core.
 *
 * Chunking, allocation and threading
 * ----------------------------------
 * process, reset and snapshot do NOT allocate, take no locks and make no
 * syscalls. The interpolation history is carried across process calls, so
 * true_peak_* and total_true_peak_* do not depend on how the caller chunks the
 * stream (block peaks do, exactly like dg_limiter and dg_meter). Only create
 * allocates. One handle must be driven by one thread at a time; destroy must
 * not race with process.
 *
 * Supported configuration: one or two interleaved channels, the same
 * DG_PCM_CHANNELS_MIN..DG_PCM_CHANNELS_MAX range as the rest of the core. The
 * sample rate is validated against DG_SAMPLE_RATE_MIN..DG_SAMPLE_RATE_MAX but
 * does not change the oversampling factor.
 */

#ifndef DREAMGUARD_DSP_TRUE_PEAK_H
#define DREAMGUARD_DSP_TRUE_PEAK_H

#include <stdint.h>

#include "dreamguard/pcm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed 4x reconstruction factor. The value is part of the API because tests
 * and callers report it; changing it is an ABI-visible change. */
#define DG_TRUE_PEAK_OVERSAMPLING 4

/* Group delay of the 65-tap interpolator, in input frames. Phase 0 is an exact
 * delayed copy of the input; this is the delay of that centre tap. */
#define DG_TRUE_PEAK_FILTER_LATENCY 8

/* Peak levels below this are clamped before the dB conversion, so silence maps
 * to -240 dBFS and never to -INFINITY. */
#define DG_TRUE_PEAK_LEVEL_FLOOR (1e-12)

/* Opaque analyser handle. Never dereference or reinterpret it. */
typedef struct dg_true_peak dg_true_peak;

typedef struct dg_true_peak_config {
    int32_t sample_rate;   /* frames per second, [DG_SAMPLE_RATE_MIN, DG_SAMPLE_RATE_MAX] */
    int32_t channel_count; /* interleaved channels, [DG_PCM_CHANNELS_MIN, DG_PCM_CHANNELS_MAX] */
} dg_true_peak_config;

/* Result of the most recent process call plus the totals since create/reset.
 *
 * The block fields describe the last non-empty dg_true_peak_process call; a
 * zero-frame call leaves them unchanged. The total fields and the counters
 * accumulate until dg_true_peak_reset. The dBFS fields use
 * DG_TRUE_PEAK_LEVEL_FLOOR for silence and are always finite. */
typedef struct dg_true_peak_values {
    double sample_peak_linear;       /* max |x| in the last non-empty block */
    double sample_peak_dbfs;         /* 20*log10(sample_peak_linear), floored */
    double true_peak_linear;         /* max |y| of the 4x reconstruction in the last block */
    double true_peak_dbfs;           /* 20*log10(true_peak_linear), floored */
    double total_sample_peak_linear; /* max |x| since create/reset */
    double total_sample_peak_dbfs;
    double total_true_peak_linear;   /* max |y| since create/reset */
    double total_true_peak_dbfs;
    uint64_t frames;                 /* input frames processed since create/reset */
    uint64_t non_finite_samples;     /* NaN/Inf channel-samples replaced with 0 */
    int32_t channel_count;           /* configured channel count */
    int32_t oversampling_factor;     /* always DG_TRUE_PEAK_OVERSAMPLING */
    uint32_t filter_taps;            /* total interpolation taps (65) */
    uint32_t filter_latency_frames;  /* DG_TRUE_PEAK_FILTER_LATENCY */
} dg_true_peak_values;

/* Creates an analyser for one stream.
 *
 * On any failure *out_true_peak is set to NULL:
 *   DG_ERR_INVALID_ARGUMENT            config or out_true_peak was NULL
 *   DG_ERR_SAMPLE_RATE_OUT_OF_RANGE    sample_rate outside the documented range
 *   DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE  channel_count outside [1, 2]
 *   DG_ERR_OUT_OF_MEMORY               the handle could not be allocated
 *
 * Validation order is sample rate, then channel count. */
dg_status dg_true_peak_create(const dg_true_peak_config* config,
                              dg_true_peak** out_true_peak);

/* Releases a handle obtained from dg_true_peak_create exactly once.
 *   DG_OK              released
 *   DG_ERR_NULL_HANDLE true_peak was NULL */
dg_status dg_true_peak_destroy(dg_true_peak* true_peak);

/* Clears the interpolation history, the block/total peaks and every counter.
 * The configuration survives. */
dg_status dg_true_peak_reset(dg_true_peak* true_peak);

/* Analyses frame_count interleaved frames. The view must describe the same
 * channel count the analyser was created with; frame_count may be 0 (a no-op
 * that accepts any channel count, like the other DSP processors).
 *
 * The analyser only reads the input; there is no output buffer and therefore
 * no aliasing contract beyond the usual "the view really holds
 * frame_count * channel_count floats" promise.
 *
 *   DG_OK                   frames were analysed
 *   DG_ERR_INVALID_ARGUMENT the view was invalid, its channel count differs, or
 *                           its sample pointer was NULL with frame_count > 0
 *   DG_ERR_NULL_HANDLE      true_peak was NULL */
dg_status dg_true_peak_process(dg_true_peak* true_peak, dg_pcm_view input);

/* Writes the current values (see dg_true_peak_values). O(1); does not allocate.
 *   DG_OK                   *out_values was written
 *   DG_ERR_INVALID_ARGUMENT out_values was NULL
 *   DG_ERR_NULL_HANDLE      true_peak was NULL */
dg_status dg_true_peak_snapshot(const dg_true_peak* true_peak,
                                dg_true_peak_values* out_values);

/* Pure helper: 20*log10(linear) with the DG_TRUE_PEAK_LEVEL_FLOOR clamp, so the
 * result is finite for 0 and negative inputs. NaN maps to NaN. Needs no
 * handle. */
double dg_true_peak_linear_to_dbfs(double linear);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DREAMGUARD_DSP_TRUE_PEAK_H */
