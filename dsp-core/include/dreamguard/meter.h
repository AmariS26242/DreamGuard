/* DreamGuard DSP core - streaming Float32 PCM metering (DG-012).
 *
 * A dg_meter is a stateful analyser for one interleaved Float32 stream. It is
 * analysis only: it never changes a sample, allocates nothing after create, takes
 * no locks, and is not part of the playback path (nothing in the JNI bridge calls
 * it yet).
 *
 * What is measured
 * ----------------
 *   block_*   the frames of the most recent non-empty dg_meter_process() call;
 *   window_*  the rolling metering window (window_ms, default 400 ms);
 *   total_*   every frame since create/reset;
 *   peak_hold the largest |x| since create/reset (or dg_meter_reset_peak_hold).
 *
 * Peak means SAMPLE peak, max |x| of the samples that are actually present. It is
 * not a true/inter-sample peak; that belongs to DG-014. RMS means
 * sqrt(mean(x^2)). Both are linear magnitudes; use dg_meter_linear_to_dbfs() to
 * display them.
 *
 * dBFS convention: dBFS = 20*log10(value). A full-scale sine therefore reads
 * 0.0 peak and -3.01 dBFS RMS. This is deliberately NOT the AES17 "+3 dB" peak
 * convention.
 *
 * Numeric boundaries
 * ------------------
 *   - zero input: RMS = 0, peak = 0, dBFS = -INFINITY (never a tiny epsilon);
 *   - non-finite samples (NaN, +Inf, -Inf) are not trusted: each is counted in
 *     values.non_finite_samples and contributes 0 to energy, to peak and to the
 *     window ring. No value returned by snapshot is NaN or Inf because of a bad
 *     sample; a block of only NaN reports RMS 0 and peak 0, and the counter says
 *     the input was broken;
 *   - clip count: finite samples with |x| > 1.0 exactly (|x| == 1.0 is not a
 *     clip). Non-finite samples are not clips; they are counted separately;
 *   - the rolling window stores |x| as float, so its mean square is accumulated
 *     from float magnitudes in double. Magnitudes below ~1e-45 (about -316 dBFS)
 *     are not representable and contribute 0.
 *
 * Windows and chunking
 * --------------------
 * The window length is round(sample_rate * window_ms / 1000) frames, at least 1.
 * Memory is window_frames * channel_count * 4 bytes, allocated once by create:
 * 19 200 frames * 2 channels * 4 B = 153.6 kB at 48 kHz / 400 ms, and at most
 * 1.5 MB at 192 kHz / 1000 ms stereo. A zero-frame process call is a no-op and
 * leaves every value (including block_*) unchanged. Block values depend on how
 * the caller chunks the stream; window and total values do not (their summation
 * order is fixed), which is what the chunking test checks.
 *
 * Threading
 * ---------
 * All functions on one handle must be externally serialized (normally the audio
 * thread owns the handle). There are no locks, atomics or reference counts.
 */

#ifndef DREAMGUARD_DSP_METER_H
#define DREAMGUARD_DSP_METER_H

#include <stdint.h>

#include "dreamguard/pcm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DG_METER_DEFAULT_WINDOW_MS 400
#define DG_METER_WINDOW_MS_MIN 1
#define DG_METER_WINDOW_MS_MAX 1000

/* Opaque meter handle. Never dereference or reinterpret it. */
typedef struct dg_meter dg_meter;

typedef struct dg_meter_config {
    int32_t sample_rate;   /* frames per second, [DG_SAMPLE_RATE_MIN, DG_SAMPLE_RATE_MAX] */
    int32_t channel_count; /* interleaved channels, [DG_PCM_CHANNELS_MIN, DG_PCM_CHANNELS_MAX] */
    int32_t window_ms;     /* rolling RMS window in ms, 0 selects DG_METER_DEFAULT_WINDOW_MS,
                            * otherwise [DG_METER_WINDOW_MS_MIN, DG_METER_WINDOW_MS_MAX] */
} dg_meter_config;

/* Snapshot of every meter value. Entries with index >= channel_count are
 * always 0.0F. The window counters describe the rolling window, not the block. */
typedef struct dg_meter_values {
    float block_peak[2];          /* max |x| in the most recent non-empty block */
    float block_rms[2];           /* RMS of the most recent non-empty block */
    float window_peak[2];         /* max |x| inside the rolling window */
    float window_rms[2];          /* RMS inside the rolling window */
    float peak_hold[2];           /* max |x| since create/reset (or reset_peak_hold) */
    float total_rms[2];           /* RMS over every frame since create/reset */
    uint64_t total_frames;        /* frames processed since create/reset */
    uint64_t clip_samples;        /* finite samples with |x| > 1.0 since create/reset */
    uint64_t non_finite_samples;  /* NaN/Inf samples seen since create/reset */
    uint32_t window_fill_frames;  /* frames currently inside the window (<= capacity) */
    uint32_t window_capacity_frames; /* configured window length in frames */
} dg_meter_values;

/* Creates a meter for one stream.
 *
 * On any failure *out_meter is set to NULL and a specific code is returned:
 *   DG_ERR_INVALID_ARGUMENT            config or out_meter was NULL, or window_ms
 *                                      was non-zero and outside its range
 *   DG_ERR_SAMPLE_RATE_OUT_OF_RANGE    sample_rate outside the documented range
 *   DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE  channel_count outside [1, 2]
 *   DG_ERR_OUT_OF_MEMORY               the ring buffer or handle allocation failed
 *
 * Validation order is sample rate, then channel count, then window length. */
dg_status dg_meter_create(const dg_meter_config* config, dg_meter** out_meter);

/* Releases a handle obtained from dg_meter_create exactly once.
 *   DG_OK                the meter was released
 *   DG_ERR_NULL_HANDLE   meter was NULL */
dg_status dg_meter_destroy(dg_meter* meter);

/* Clears every accumulated value and the rolling window. Configuration and the
 * window allocation survive, so a reset handle behaves like a freshly created
 * one for the same stream. */
dg_status dg_meter_reset(dg_meter* meter);

/* Clears only peak_hold[] (both channels). The rolling window, block values,
 * totals and counters are untouched. */
dg_status dg_meter_reset_peak_hold(dg_meter* meter);

/* Analyses frame_count interleaved frames. The view must describe the same
 * channel count the meter was created with; frame_count may be 0 (a no-op).
 *
 * Rejections are atomic: an invalid view or a channel-count mismatch returns
 * DG_ERR_INVALID_ARGUMENT and changes nothing. The view's sample pointer must
 * hold frame_count * channel_count floats (the same promise dg_pcm_view makes).
 *   DG_OK                  the frames were analysed
 *   DG_ERR_INVALID_ARGUMENT the view was invalid or its channel count differs
 *   DG_ERR_NULL_HANDLE     meter was NULL */
dg_status dg_meter_process(dg_meter* meter, dg_pcm_view input);

/* Writes the current values. Safe to call with a zero-frame history: everything
 * is 0, and the window is empty. The rolling window is scanned here, so a
 * snapshot is O(window_capacity * channel_count); call it at display rate, not
 * per audio block.
 *   DG_OK                  *out_values was written
 *   DG_ERR_INVALID_ARGUMENT out_values was NULL
 *   DG_ERR_NULL_HANDLE     meter was NULL */
dg_status dg_meter_snapshot(const dg_meter* meter, dg_meter_values* out_values);

/* Pure helper: 20*log10(linear) as float.
 *   0 and -0 -> -INFINITY (including any negative input, which is not a valid
 *   magnitude), NaN -> NaN, +Infinity -> +Infinity.
 * This function needs no handle and takes no locks. */
float dg_meter_linear_to_dbfs(float linear);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DREAMGUARD_DSP_METER_H */
