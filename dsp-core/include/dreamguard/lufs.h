/* DreamGuard DSP core - LUFS loudness analysis (DG-012).
 *
 * STATUS: PROVISIONAL. NOT VERIFIED AGAINST A REFERENCE IMPLEMENTATION.
 *
 * The algorithm follows ITU-R BS.1770-4 (K-weighting, 400 ms gating blocks with
 * 75 % overlap, a -70 LUFS absolute gate and a -10 LU relative gate) and the
 * EBU R128 / Tech 3341 calibration convention, but no third-party tool
 * (ffmpeg ebur128, libebur128, a broadcast loudness meter, ...) has been run
 * against this workspace. Do NOT describe this component or its output as
 * "BS.1770-4 compliant" until that comparison exists and its error is recorded.
 * See docs/STATUS.md and docs/DEVLOG.md for the exact evidence status.
 *
 * Standard and units
 * ------------------
 *   - Values are LUFS (loudness units relative to full scale), computed as
 *     -0.691 + 10*log10(sum_i G_i * z_i), where z_i is the mean square of the
 *     K-weighted channel i over the window and G_i is the channel weight.
 *     Differences of loudness in LU are plain dB differences.
 *   - The -0.691 offset plus the K-weighting response make a stereo 1 kHz sine
 *     whose PEAK amplitude is A read 20*log10(A) LUFS, which is the Tech 3341
 *     calibration convention. This implementation lands within 0.03 LU of that:
 *     K(1 kHz) is about +0.667 dB while the offset is 0.691 dB, so the measured
 *     residual is about -0.024 LU. It is registered in docs/STATUS.md rather
 *     than hidden; a mono signal from the same generator reads 3.0103 LU lower,
 *     because loudness sums channel powers and mono has one channel instead of
 *     two.
 *
 * Channels
 * --------
 * Only one or two interleaved channels are supported (the same DG_CHANNELS_MIN..
 * DG_CHANNELS_MAX range as the engine). Channel weights are 1.0 for both, which
 * is the BS.1770 weight for L/R. There is no centre, surround or LFE support in
 * this component, so the 1.41 surround weight is unreachable and is not
 * implemented.
 *
 * Sample rates
 * ------------
 * 8000..192000 Hz. The two biquads are the BS.1770 analog prototype
 * discretised with the bilinear transform (the derivation libebur128 uses), so
 * the 48 kHz coefficients reproduce the published BS.1770-4 table; the tests
 * check that reproduction to 1e-6 and are the only numeric evidence this
 * workspace has. Rates other than 48 kHz are derived, not published, and the
 * whole component stays PROVISIONAL until a reference comparison exists.
 *
 * Windows and gating
 * ------------------
 *   - one hop is round(sample_rate / 10) frames (100 ms; exact at 48 kHz);
 *   - momentary loudness M uses the last 4 hops (400 ms);
 *   - short-term loudness S uses the last 30 hops (3 s) and is -INFINITY until
 *     3 s of complete hops exist;
 *   - integrated loudness I uses one 400 ms block per completed hop (75 %
 *     overlap). A block below -70 LUFS is discarded (absolute gate). From the
 *     remaining blocks the relative threshold is (loudness of their mean square)
 *     - 10 LU; blocks below it are discarded again. I is the loudness of the mean
 *     square of the survivors.
 *   - A trailing partial hop/block is not used: loudness windows are only
 *     reported once they are complete. values.pending_frames shows how many
 *     frames are waiting for the next hop.
 *   - Silence (all samples exactly zero) yields -INFINITY, never a numeric floor
 *     such as -70. A completely silent stream has integrated_ready == 0.
 *
 * Numeric boundaries
 * ------------------
 *   - non-finite samples (NaN, +Inf, -Inf) are counted in non_finite_samples and
 *     replaced with 0 before filtering, so one bad sample cannot poison the IIR
 *     state forever. A stream of NaN therefore reads like silence plus a
 *     non-zero non_finite_samples counter;
 *   - K-weighting is processed in double precision and the per-block energies
 *     are stored as double. Input samples are float; no extended precision type
 *     is used, so the ABI is the same on 32-bit and 64-bit targets;
 *   - the block store is a fixed allocation of DG_LUFS_BLOCKS_CAPACITY entries
 *     (36 000 blocks = one hour at 10 blocks/s) used as a ring: once full, the
 *     oldest block is overwritten and counted in blocks_dropped, so the
 *     integrated value always covers the most recent hour and the analysis path
 *     never reallocates.
 *   - values in this header are never NaN: insufficient data, silence and
 *     empty windows are all reported as -INFINITY.
 *
 * Threading
 * ---------
 * All functions on one handle must be externally serialized. There are no locks,
 * atomics or reference counts. dg_lufs_snapshot() walks the stored blocks and is
 * O(blocks); call it at display rate.
 */

#ifndef DREAMGUARD_DSP_LUFS_H
#define DREAMGUARD_DSP_LUFS_H

#include <stdint.h>

#include "dreamguard/pcm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Number of 400 ms gating blocks the integrated loudness can hold. At the
 * standard 10 blocks/s this is one hour; the block store is allocated once. */
#define DG_LUFS_BLOCKS_CAPACITY 36000
#define DG_LUFS_HOP_MS 100
#define DG_LUFS_MOMENTARY_MS 400
#define DG_LUFS_SHORT_TERM_MS 3000
#define DG_LUFS_ABSOLUTE_GATE_LUFS (-70.0)
#define DG_LUFS_RELATIVE_GATE_LU (-10.0)
#define DG_LUFS_LOUDNESS_OFFSET (-0.691)

/* Opaque LUFS analyser handle. Never dereference or reinterpret it. */
typedef struct dg_lufs dg_lufs;

typedef struct dg_lufs_config {
    int32_t sample_rate;   /* frames per second, [DG_SAMPLE_RATE_MIN, DG_SAMPLE_RATE_MAX] */
    int32_t channel_count; /* interleaved channels, [DG_PCM_CHANNELS_MIN, DG_PCM_CHANNELS_MAX] */
} dg_lufs_config;

/* Normalised biquad: a0 is 1, so y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2]
 *                                        - a1*y[n-1] - a2*y[n-2]. */
typedef struct dg_biquad_coefficients {
    double b0, b1, b2, a1, a2;
} dg_biquad_coefficients;

/* The two K-weighting stages used by this component. */
typedef struct dg_lufs_k_weighting {
    dg_biquad_coefficients shelf;     /* BS.1770-4 stage 1: high shelf */
    dg_biquad_coefficients high_pass; /* BS.1770-4 stage 2: RLB high-pass */
} dg_lufs_k_weighting;

/* Result of the last analysis. Values are never NaN; "not available" is
 * -INFINITY. */
typedef struct dg_lufs_values {
    double momentary_lufs;            /* last complete 400 ms window, or -INFINITY */
    double short_term_lufs;           /* last complete 3 s window, or -INFINITY */
    double integrated_lufs;           /* gated integrated loudness, or -INFINITY */
    uint64_t blocks;                  /* gating blocks produced (including dropped) */
    uint64_t blocks_after_absolute_gate;
    uint64_t blocks_used;             /* survivors of the relative gate */
    uint64_t blocks_dropped;          /* oldest blocks overwritten in the ring store */
    uint64_t non_finite_samples;      /* NaN/Inf samples replaced with 0 */
    uint32_t hop_frames;              /* frames per 100 ms hop (rounded to 1 frame) */
    uint32_t block_frames;            /* frames per 400 ms block (4 hops) */
    uint32_t pending_frames;          /* frames accumulated toward the next hop */
    int32_t integrated_ready;         /* 1 when integrated_lufs is finite */
} dg_lufs_values;

/* Creates a LUFS analyser for one stream.
 *
 *   DG_ERR_INVALID_ARGUMENT            config or out_lufs was NULL
 *   DG_ERR_SAMPLE_RATE_OUT_OF_RANGE    sample_rate outside the documented range
 *   DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE  channel_count outside [1, 2]
 *   DG_ERR_OUT_OF_MEMORY               the block store or handle allocation failed
 *
 * On any failure *out_lufs is set to NULL. Validation order is sample rate, then
 * channel count. */
dg_status dg_lufs_create(const dg_lufs_config* config, dg_lufs** out_lufs);

/* Releases a handle obtained from dg_lufs_create exactly once.
 *   DG_OK                the analyser was released
 *   DG_ERR_NULL_HANDLE   lufs was NULL */
dg_status dg_lufs_destroy(dg_lufs* lufs);

/* Clears the K-weighting state, hop history, block store and every counter. The
 * sample rate, channel count and coefficients survive. */
dg_status dg_lufs_reset(dg_lufs* lufs);

/* Analyses frame_count interleaved frames. The view must describe the same
 * channel count the analyser was created with; frame_count may be 0 (a no-op).
 * Rejections are atomic and change nothing. */
dg_status dg_lufs_process(dg_lufs* lufs, dg_pcm_view input);

/* Writes the current values (see dg_lufs_values). O(blocks).
 *   DG_OK                  *out_values was written
 *   DG_ERR_INVALID_ARGUMENT out_values was NULL
 *   DG_ERR_NULL_HANDLE     lufs was NULL */
dg_status dg_lufs_snapshot(const dg_lufs* lufs, dg_lufs_values* out_values);

/* Pure query for the K-weighting coefficients this component would use at a
 * given sample rate, without creating a handle. Useful to inspect or regress the
 * filter design; 48 000 Hz must reproduce the published BS.1770-4 table.
 *   DG_OK                  *out_weighting was written
 *   DG_ERR_INVALID_ARGUMENT out_weighting was NULL
 *   DG_ERR_SAMPLE_RATE_OUT_OF_RANGE sample_rate outside the documented range */
dg_status dg_lufs_k_weighting_for_rate(int32_t sample_rate,
                                       dg_lufs_k_weighting* out_weighting);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DREAMGUARD_DSP_LUFS_H */
