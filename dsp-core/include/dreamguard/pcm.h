/* DreamGuard DSP core - portable Float32 PCM helpers (DG-011).
 *
 * This header is the stateless half of the native audio code. It converts and
 * copies interleaved Float32 PCM in memory; it does not open a device, does not
 * run a render loop, and shares no state with the soundscape engine in
 * dreamguard/dsp_core.h. In particular:
 *   - it is NOT an audio capture implementation (no AudioRecord, no OpenSL/AAudio,
 *     no microphone permission path) and nothing here claims third-party capture;
 *   - it is NOT the soundscape generator; the generator is DG-010's dg_render.
 *
 * Layout: samples are interleaved Float32, frame-major, channel-minor, exactly
 * the layout Android's ENCODING_PCM_FLOAT AudioTrack consumes. Supported channel
 * counts are DG_PCM_CHANNELS_MIN..DG_PCM_CHANNELS_MAX (mono and stereo). Frame
 * counts are arbitrary, including 0; operations never allocate and never touch
 * memory outside the buffers described by the caller. A descriptor promises that
 * its pointer really holds frame_count * channel_count floats.
 *
 * Every function validates both buffer descriptors and reports
 * DG_ERR_INVALID_ARGUMENT for NULL sample pointers with a non-zero frame count,
 * unsupported channel counts, mismatched frame counts where the operation needs
 * them, or a layout whose sample count cannot be represented.
 *
 * Buffer aliasing (defined, not left to the C library):
 *   - Identical in/out pointer is ALLOWED when the sample count does not change:
 *     dg_pcm_bypass (a validated no-op), dg_pcm_apply_gain (element-wise, one
 *     read and one write per sample) and dg_pcm_map_channels with equal channel
 *     counts (also a no-op). These are true in-place operations.
 *   - A channel-count conversion (mono -> stereo or stereo -> mono) changes the
 *     number of samples, so its buffers must be disjoint; the identical pointer
 *     is rejected too, because an up-mix would overwrite its own input.
 *   - Distinct buffers whose sample ranges partially overlap are rejected with
 *     DG_ERR_INVALID_ARGUMENT for every function, and no sample is written.
 *   - A zero-frame block touches no memory, so aliasing is irrelevant there.
 *
 * The sample-count arithmetic inside the implementation is widened to size_t
 * before multiplying (frame * 2 must not wrap in 32-bit math on a 64-bit host).
 */

#ifndef DREAMGUARD_DSP_PCM_H
#define DREAMGUARD_DSP_PCM_H

#include <stdint.h>

#include "dreamguard/dsp_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DG_PCM_CHANNELS_MIN 1
#define DG_PCM_CHANNELS_MAX 2

/* Read-only view of an interleaved Float32 PCM block. */
typedef struct dg_pcm_view {
    const float* samples;   /* frame_count * channel_count floats, may be NULL iff frame_count == 0 */
    uint32_t frame_count;   /* frames, may be 0 */
    int32_t channel_count;  /* [DG_PCM_CHANNELS_MIN, DG_PCM_CHANNELS_MAX] */
} dg_pcm_view;

/* Writable interleaved Float32 PCM block with the same layout rules. */
typedef struct dg_pcm_buffer {
    float* samples;
    uint32_t frame_count;
    int32_t channel_count;
} dg_pcm_buffer;

/* Copies input to output unchanged, bit for bit.
 *
 * Both blocks must have the same frame_count and channel_count. Every 32-bit
 * pattern is preserved, including NaN payloads, infinities and signed zero -
 * this is what makes bypass a true pass-through rather than a "close enough"
 * copy, and it is the acceptance property DG-011 asks for.
 * In-place: output.samples == input.samples (same shape) is allowed and is a
 * validated no-op; overlapping distinct buffers are rejected.
 *   DG_OK                   output holds an exact copy of input
 *   DG_ERR_INVALID_ARGUMENT a buffer descriptor was invalid, the shapes differ,
 *                           or the two buffers partially overlap */
dg_status dg_pcm_bypass(dg_pcm_view input, dg_pcm_buffer output);

/* Multiplies every sample by gain.
 *
 * gain must be finite and in [0, 1]; anything else is rejected instead of being
 * silently clamped, because a caller that asks for 2.0 or NaN wants to know.
 * With gain == 1.0 the result is bit-identical to dg_pcm_bypass for finite
 * inputs. A silent block stays silent.
 * In-place: the sample is read once and written once, so identical in/out
 * pointers are allowed; overlapping distinct buffers are rejected.
 *   DG_OK                   output holds input * gain
 *   DG_ERR_INVALID_ARGUMENT gain was non-finite or outside [0, 1], a buffer
 *                           descriptor was invalid, the shapes differ, or the
 *                           buffers partially overlap */
dg_status dg_pcm_apply_gain(dg_pcm_view input, dg_pcm_buffer output, float gain);

/* Copies input to output, converting the channel layout when it differs.
 *
 * Both blocks must have the same frame_count; each channel_count must be 1 or 2:
 *   mono   -> mono    bit-exact copy (in place allowed)
 *   stereo -> stereo  bit-exact copy (in place allowed)
 *   mono   -> stereo  the mono sample is duplicated into both channels
 *   stereo -> mono    (left + right) * 0.5
 * The mono up-mix is lossless; the stereo down-mix is not, because it sums two
 * channels (and rounding happens once, in the mix).
 * In-place: only when the channel counts are equal. A channel-count conversion
 * is rejected if the buffers overlap at all, including the identical pointer.
 *   DG_OK                   output holds the converted block
 *   DG_ERR_INVALID_ARGUMENT a buffer descriptor was invalid, the frame counts
 *                           differ, a channel count is unsupported, or the
 *                           buffers alias across a channel-count conversion */
dg_status dg_pcm_map_channels(dg_pcm_view input, dg_pcm_buffer output);

/* Reports whether every sample is exactly 0.0F (either +0.0F or -0.0F counts as
 * silence; NaN and denormals do not).
 *   DG_OK                   *out_silent was written (1 silent, 0 not silent);
 *                            a zero-frame block is silent
 *   DG_ERR_INVALID_ARGUMENT out_silent was NULL, or input was invalid */
dg_status dg_pcm_is_silent(dg_pcm_view input, int* out_silent);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DREAMGUARD_DSP_PCM_H */
