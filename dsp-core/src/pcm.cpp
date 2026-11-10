/* Portable Float32 PCM helpers (DG-011).
 *
 * Stateless on purpose: no allocation, no globals, no synchronization, so the
 * same functions are usable from an audio thread without a lock. See
 * include/dreamguard/pcm.h for the contract, including the aliasing rules: an
 * identical in/out pointer is a defined in-place operation only where the sample
 * count does not change; overlapping distinct buffers are rejected instead of
 * being handed to memcpy (which is undefined for overlap).
 */

#include "dreamguard/pcm.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

bool layout_is_representable(uint32_t frame_count, int32_t channel_count) {
    if (channel_count < DG_PCM_CHANNELS_MIN || channel_count > DG_PCM_CHANNELS_MAX) {
        return false;
    }
    const auto channels = static_cast<uint64_t>(static_cast<uint32_t>(channel_count));
    const uint64_t samples = static_cast<uint64_t>(frame_count) * channels;
    return samples <= static_cast<uint64_t>(SIZE_MAX / sizeof(float));
}

bool view_is_valid(dg_pcm_view view) {
    if (!layout_is_representable(view.frame_count, view.channel_count)) {
        return false;
    }
    return view.samples != nullptr || view.frame_count == 0;
}

bool buffer_is_valid(dg_pcm_buffer buffer) {
    if (!layout_is_representable(buffer.frame_count, buffer.channel_count)) {
        return false;
    }
    return buffer.samples != nullptr || buffer.frame_count == 0;
}

std::size_t sample_count(uint32_t frame_count, int32_t channel_count) {
    // Widen before multiplying: frame * 2 in 32-bit math would wrap for large
    // frame counts even on a 64-bit host.
    return static_cast<std::size_t>(frame_count) * static_cast<std::size_t>(channel_count);
}

bool same_shape(uint32_t input_frames, int32_t input_channels,
                uint32_t output_frames, int32_t output_channels) {
    return input_frames == output_frames && input_channels == output_channels;
}

// Byte ranges [begin, begin + bytes) intersect? `bytes` is already known to be
// representable as a sample count * sizeof(float); the uintptr_t additions are
// checked so a wraparound can never be mistaken for disjointness.
bool ranges_intersect(const void* a, std::size_t a_bytes, const void* b,
                      std::size_t b_bytes) {
    if (a_bytes == 0 || b_bytes == 0) {
        return false;
    }
    const auto a_begin = reinterpret_cast<std::uintptr_t>(a);
    const auto b_begin = reinterpret_cast<std::uintptr_t>(b);
    const auto a_end = a_begin + a_bytes;
    const auto b_end = b_begin + b_bytes;
    if (a_end < a_begin || b_end < b_begin) {
        return true;  // cannot prove disjointness: treat as aliasing
    }
    return a_begin < b_end && b_begin < a_end;
}

bool in_place(const float* input, float* output) {
    return static_cast<const void*>(input) == static_cast<const void*>(output);
}

}  // namespace

extern "C" {

dg_status dg_pcm_bypass(dg_pcm_view input, dg_pcm_buffer output) {
    if (!view_is_valid(input) || !buffer_is_valid(output) ||
        !same_shape(input.frame_count, input.channel_count, output.frame_count,
                    output.channel_count)) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (input.frame_count == 0) {
        return DG_OK;
    }
    if (in_place(input.samples, output.samples)) {
        return DG_OK;  // already the identity; memcpy on equal pointers is UB
    }
    const std::size_t bytes = sample_count(input.frame_count, input.channel_count) * sizeof(float);
    if (ranges_intersect(input.samples, bytes, output.samples, bytes)) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    std::memcpy(output.samples, input.samples, bytes);
    return DG_OK;
}

dg_status dg_pcm_apply_gain(dg_pcm_view input, dg_pcm_buffer output, float gain) {
    if (!view_is_valid(input) || !buffer_is_valid(output) ||
        !same_shape(input.frame_count, input.channel_count, output.frame_count,
                    output.channel_count)) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (!(gain >= 0.0F) || !(gain <= 1.0F)) {  // also rejects NaN and infinities
        return DG_ERR_INVALID_ARGUMENT;
    }
    const std::size_t count = sample_count(input.frame_count, input.channel_count);
    if (!in_place(input.samples, output.samples) &&
        ranges_intersect(input.samples, count * sizeof(float), output.samples,
                         count * sizeof(float))) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    for (std::size_t i = 0; i < count; ++i) {
        output.samples[i] = input.samples[i] * gain;
    }
    return DG_OK;
}

dg_status dg_pcm_map_channels(dg_pcm_view input, dg_pcm_buffer output) {
    if (!view_is_valid(input) || !buffer_is_valid(output)) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (input.frame_count != output.frame_count) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (input.frame_count == 0) {
        return DG_OK;
    }

    const bool same_channels = input.channel_count == output.channel_count;
    const std::size_t input_bytes =
        sample_count(input.frame_count, input.channel_count) * sizeof(float);
    const std::size_t output_bytes =
        sample_count(output.frame_count, output.channel_count) * sizeof(float);
    if (in_place(input.samples, output.samples)) {
        // Equal channel counts: identical layout, nothing to do. A channel-count
        // conversion cannot run in place (an up-mix would overwrite its input).
        return same_channels ? DG_OK : DG_ERR_INVALID_ARGUMENT;
    }
    if (ranges_intersect(input.samples, input_bytes, output.samples, output_bytes)) {
        return DG_ERR_INVALID_ARGUMENT;
    }

    if (same_channels) {
        std::memcpy(output.samples, input.samples, input_bytes);
        return DG_OK;
    }

    if (input.channel_count == 1) {
        for (std::uint32_t frame = 0; frame < input.frame_count; ++frame) {
            const float sample = input.samples[frame];
            const std::size_t out = static_cast<std::size_t>(frame) * 2;
            output.samples[out] = sample;
            output.samples[out + 1] = sample;
        }
        return DG_OK;
    }
    for (std::uint32_t frame = 0; frame < input.frame_count; ++frame) {
        const std::size_t in = static_cast<std::size_t>(frame) * 2;
        const float left = input.samples[in];
        const float right = input.samples[in + 1];
        output.samples[frame] = (left + right) * 0.5F;
    }
    return DG_OK;
}

dg_status dg_pcm_is_silent(dg_pcm_view input, int* out_silent) {
    if (out_silent == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (!view_is_valid(input)) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    const std::size_t count = sample_count(input.frame_count, input.channel_count);
    for (std::size_t i = 0; i < count; ++i) {
        if (input.samples[i] != 0.0F) {
            *out_silent = 0;
            return DG_OK;
        }
    }
    *out_silent = 1;
    return DG_OK;
}

}  // extern "C"
