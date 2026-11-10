/* Streaming Float32 PCM metering (DG-012).
 *
 * Stateless-free, allocation-free after create. The only shared state is the
 * caller-owned handle; there are no globals, locks or atomics. See
 * include/dreamguard/meter.h for the public contract and numeric policy.
 */

#include "dreamguard/meter.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

namespace {

constexpr int32_t kMaxChannels = 2;

bool layout_is_representable(uint32_t frame_count, int32_t channel_count) {
    if (channel_count < DG_PCM_CHANNELS_MIN || channel_count > DG_PCM_CHANNELS_MAX) {
        return false;
    }
    const uint64_t samples = static_cast<uint64_t>(frame_count) *
                             static_cast<uint64_t>(static_cast<uint32_t>(channel_count));
    return samples <= static_cast<uint64_t>(SIZE_MAX / sizeof(float));
}

bool view_is_valid(dg_pcm_view view) {
    if (!layout_is_representable(view.frame_count, view.channel_count)) {
        return false;
    }
    return view.samples != nullptr || view.frame_count == 0;
}

uint32_t window_frames_for(int32_t sample_rate, int32_t window_ms) {
    const double frames =
        static_cast<double>(sample_rate) * static_cast<double>(window_ms) / 1000.0;
    const double rounded = std::floor(frames + 0.5);
    return rounded < 1.0 ? 1U : static_cast<uint32_t>(rounded);
}

}  // namespace

struct dg_meter {
    int32_t sample_rate = 0;
    int32_t channel_count = 0;
    uint32_t window_capacity = 0;
    uint32_t window_fill = 0;
    uint32_t ring_pos = 0;
    float* ring = nullptr;  // window_capacity * channel_count magnitudes
    double sumsq[kMaxChannels] = {};
    uint64_t total_frames = 0;
    uint64_t clip_samples = 0;
    uint64_t non_finite_samples = 0;
    float block_peak[kMaxChannels] = {};
    float block_rms[kMaxChannels] = {};
    float peak_hold[kMaxChannels] = {};
};

namespace {

void clear_values(dg_meter* meter) {
    for (int32_t ch = 0; ch < kMaxChannels; ++ch) {
        meter->sumsq[ch] = 0.0;
        meter->block_peak[ch] = 0.0F;
        meter->block_rms[ch] = 0.0F;
        meter->peak_hold[ch] = 0.0F;
    }
    meter->total_frames = 0;
    meter->clip_samples = 0;
    meter->non_finite_samples = 0;
    meter->window_fill = 0;
    meter->ring_pos = 0;
    const std::size_t samples = static_cast<std::size_t>(meter->window_capacity) *
                                static_cast<std::size_t>(meter->channel_count);
    for (std::size_t i = 0; i < samples; ++i) {
        meter->ring[i] = 0.0F;
    }
}

}  // namespace

extern "C" {

dg_status dg_meter_create(const dg_meter_config* config, dg_meter** out_meter) {
    if (out_meter == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    *out_meter = nullptr;
    if (config == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (config->sample_rate < DG_SAMPLE_RATE_MIN ||
        config->sample_rate > DG_SAMPLE_RATE_MAX) {
        return DG_ERR_SAMPLE_RATE_OUT_OF_RANGE;
    }
    if (config->channel_count < DG_PCM_CHANNELS_MIN ||
        config->channel_count > DG_PCM_CHANNELS_MAX) {
        return DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE;
    }
    const int32_t window_ms =
        config->window_ms == 0 ? DG_METER_DEFAULT_WINDOW_MS : config->window_ms;
    if (window_ms < DG_METER_WINDOW_MS_MIN || window_ms > DG_METER_WINDOW_MS_MAX) {
        return DG_ERR_INVALID_ARGUMENT;
    }

    const uint32_t window_capacity = window_frames_for(config->sample_rate, window_ms);
    const std::size_t ring_samples = static_cast<std::size_t>(window_capacity) *
                                     static_cast<std::size_t>(config->channel_count);
    float* ring = new (std::nothrow) float[ring_samples];
    if (ring == nullptr) {
        return DG_ERR_OUT_OF_MEMORY;
    }
    dg_meter* meter = new (std::nothrow) dg_meter{};
    if (meter == nullptr) {
        delete[] ring;
        return DG_ERR_OUT_OF_MEMORY;
    }
    meter->sample_rate = config->sample_rate;
    meter->channel_count = config->channel_count;
    meter->window_capacity = window_capacity;
    meter->ring = ring;
    clear_values(meter);
    *out_meter = meter;
    return DG_OK;
}

dg_status dg_meter_destroy(dg_meter* meter) {
    if (meter == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    delete[] meter->ring;
    delete meter;
    return DG_OK;
}

dg_status dg_meter_reset(dg_meter* meter) {
    if (meter == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    clear_values(meter);
    return DG_OK;
}

dg_status dg_meter_reset_peak_hold(dg_meter* meter) {
    if (meter == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    for (int32_t ch = 0; ch < kMaxChannels; ++ch) {
        meter->peak_hold[ch] = 0.0F;
    }
    return DG_OK;
}

dg_status dg_meter_process(dg_meter* meter, dg_pcm_view input) {
    if (meter == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    if (!view_is_valid(input) || input.channel_count != meter->channel_count) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (input.frame_count == 0) {
        return DG_OK;
    }

    const uint32_t frames = input.frame_count;
    const int32_t channels = meter->channel_count;
    double block_energy[kMaxChannels] = {};
    float block_peak[kMaxChannels] = {};

    for (uint32_t frame = 0; frame < frames; ++frame) {
        const std::size_t base =
            static_cast<std::size_t>(frame) * static_cast<std::size_t>(channels);
        for (int32_t ch = 0; ch < channels; ++ch) {
            float sample = input.samples[base + static_cast<std::size_t>(ch)];
            if (!std::isfinite(sample)) {
                ++meter->non_finite_samples;
                sample = 0.0F;
            } else if (std::fabs(sample) > 1.0F) {
                ++meter->clip_samples;
            }
            const float magnitude = std::fabs(sample);
            const double square = static_cast<double>(sample) * static_cast<double>(sample);
            block_energy[ch] += square;
            meter->sumsq[ch] += square;
            if (magnitude > block_peak[ch]) {
                block_peak[ch] = magnitude;
            }
            meter->ring[static_cast<std::size_t>(meter->ring_pos) *
                            static_cast<std::size_t>(channels) +
                        static_cast<std::size_t>(ch)] = magnitude;
        }
        meter->ring_pos = (meter->ring_pos + 1U) % meter->window_capacity;
        if (meter->window_fill < meter->window_capacity) {
            ++meter->window_fill;
        }
    }

    meter->total_frames += frames;
    for (int32_t ch = 0; ch < channels; ++ch) {
        meter->block_peak[ch] = block_peak[ch];
        meter->block_rms[ch] =
            static_cast<float>(std::sqrt(block_energy[ch] / static_cast<double>(frames)));
        if (block_peak[ch] > meter->peak_hold[ch]) {
            meter->peak_hold[ch] = block_peak[ch];
        }
    }
    return DG_OK;
}

dg_status dg_meter_snapshot(const dg_meter* meter, dg_meter_values* out_values) {
    if (out_values == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (meter == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }

    dg_meter_values values{};
    values.total_frames = meter->total_frames;
    values.clip_samples = meter->clip_samples;
    values.non_finite_samples = meter->non_finite_samples;
    values.window_fill_frames = meter->window_fill;
    values.window_capacity_frames = meter->window_capacity;

    const int32_t channels = meter->channel_count;
    const uint32_t oldest =
        (meter->ring_pos + meter->window_capacity - meter->window_fill) %
        meter->window_capacity;
    for (int32_t ch = 0; ch < channels; ++ch) {
        values.block_peak[ch] = meter->block_peak[ch];
        values.block_rms[ch] = meter->block_rms[ch];
        values.peak_hold[ch] = meter->peak_hold[ch];
        values.total_rms[ch] =
            meter->total_frames == 0
                ? 0.0F
                : static_cast<float>(std::sqrt(
                      meter->sumsq[ch] / static_cast<double>(meter->total_frames)));

        double window_energy = 0.0;
        float window_peak = 0.0F;
        for (uint32_t k = 0; k < meter->window_fill; ++k) {
            const uint32_t index = (oldest + k) % meter->window_capacity;
            const float magnitude =
                meter->ring[static_cast<std::size_t>(index) *
                                static_cast<std::size_t>(channels) +
                            static_cast<std::size_t>(ch)];
            window_energy += static_cast<double>(magnitude) *
                             static_cast<double>(magnitude);
            if (magnitude > window_peak) {
                window_peak = magnitude;
            }
        }
        values.window_peak[ch] = window_peak;
        values.window_rms[ch] =
            meter->window_fill == 0
                ? 0.0F
                : static_cast<float>(std::sqrt(window_energy /
                                                static_cast<double>(meter->window_fill)));
    }

    *out_values = values;
    return DG_OK;
}

float dg_meter_linear_to_dbfs(float linear) {
    if (std::isnan(linear)) {
        return linear;
    }
    if (!(linear > 0.0F)) {
        return -std::numeric_limits<float>::infinity();
    }
    return static_cast<float>(20.0 * std::log10(static_cast<double>(linear)));
}

}  // extern "C"
