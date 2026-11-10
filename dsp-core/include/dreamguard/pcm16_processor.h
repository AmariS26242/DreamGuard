/* DreamGuard DSP core - PCM16 <-> compressor bridge (DG-022).
 *
 * STATUS: UNVERIFIED. This file was written for DG-022 but the change that
 * introduced it could not be compiled or run in its authoring environment, so
 * it has no execution evidence yet. The DG-013 compressor it wraps does have
 * host test evidence; see docs/STATUS.md.
 *
 * Why this exists
 * ---------------
 * The DG-013 compressor processes interleaved Float32 PCM. Android local-file
 * playback through Media3 delivers ENCODING_PCM_16BIT buffers. This component
 * is the one place where the two meet: it converts PCM16 to float, runs the
 * compressor, converts back to PCM16, and preserves the byte count (including a
 * trailing partial frame or odd byte). It exists so the JNI bridge stays a thin
 * marshalling layer and so the conversion + DSP path is testable on the host.
 *
 * It is NOT the soundscape generator, it is NOT a limiter, and it adds NO
 * lookahead: the output is sample-aligned with the input, frame for frame.
 *
 * Signal path
 * -----------
 *   int16 (native endian, interleaved) -> float = s / 32768.0 -> DG-013
 *   compressor (per block, no lookahead) -> int16 = round(clamp(y) * 32768)
 *
 * The round-to-int16 step rounds half away from zero and saturates, so a
 * positive makeup gain produces full-scale samples instead of wrapping. The
 * compressor CAN exceed 0 dBFS by design (DG-013 has no limiter); saturation is
 * the documented behaviour of this bridge, not a true-peak guarantee.
 *
 * Bypass
 * ------
 * With bypass != 0 the function copies input bytes to output verbatim with
 * memmove, so the result is bit-identical even for a partial frame, and no
 * compressor state is touched. This is the property Media3's "processing off"
 * switch relies on.
 *
 * Buffering
 * ---------
 * Any input size is accepted; the implementation converts in fixed-size chunks
 * (DG_PCM16_CHUNK_FRAMES) through a scratch array that lives inside the handle.
 * There is NO allocation on the process path, no lock and no syscall, and the
 * caller does not have to negotiate a block size. `frames` / `bytes_written`
 * in dg_pcm16_processor_values report what the call actually processed.
 *
 * A partial frame at the end of the block (bytes that do not fill a whole
 * frame) is copied to the output unchanged, so output_bytes is always equal to
 * input_bytes.
 *
 * Threading and lifetime
 * ----------------------
 * One handle is driven by one thread at a time: create/destroy/process/reset
 * must be externally serialized, exactly like dg_compressor. reset() is safe
 * between blocks, not during one. The Android layer (LocalAudioProcessor) keeps
 * the handle confined to the playback thread: configure/queueInput/flush/reset
 * all run there, so no lock is needed around the handle and no other thread can
 * observe a destroyed one.
 *
 * Channel counts are the compressor's: 1 or 2. Larger layouts are rejected at
 * create with DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE; the Android layer treats that
 * as "not supported, pass the original bytes through" instead of down-mixing.
 */

#ifndef DREAMGUARD_DSP_PCM16_PROCESSOR_H
#define DREAMGUARD_DSP_PCM16_PROCESSOR_H

#include <stdint.h>

#include "dreamguard/compressor.h"
#include "dreamguard/dsp_core.h"
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
 * by dg_compressor_create). */
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
} dg_pcm16_processor_config;

/* Result of the last dg_pcm16_processor_process call. */
typedef struct dg_pcm16_processor_values {
    uint32_t frames;           /* whole frames converted (bypass: whole frames copied) */
    uint32_t bytes_written;    /* always equal to the input byte count */
    int32_t bypassed;          /* 1 when the call was a verbatim copy */
    double gain_reduction_db;  /* compressor reduction after the call, <= 0 dB (0 in bypass) */
} dg_pcm16_processor_values;

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

/* Resets the wrapped compressor (gain back to zero dB, counters cleared); the
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
 * partial frame. The identical input/output pointer is allowed (in-place) and
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

/* Zero frames of algorithmic latency: the compressor has no lookahead. */
uint32_t dg_pcm16_processor_latency_frames(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DREAMGUARD_DSP_PCM16_PROCESSOR_H */
