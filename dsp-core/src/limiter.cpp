#include "dreamguard/limiter.h"

#include "reconstruction_filter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>

namespace {

constexpr int32_t kMaxChannels = DG_PCM_CHANNELS_MAX;
constexpr int kGuardHistory = DG_LIMITER_TRUE_PEAK_CONTEXT + 1;

bool in_range(double value, double minimum, double maximum) {
    return value >= minimum && value <= maximum;
}

/* Detector level of a sample peak, in dBFS. Silence is clamped to
 * DG_LIMITER_LEVEL_FLOOR so the value is always finite. */
double level_to_db(double peak) {
    const double clamped = peak > DG_LIMITER_LEVEL_FLOOR ? peak : DG_LIMITER_LEVEL_FLOOR;
    return 20.0 * std::log10(clamped);
}

/* One-pole release coefficient. release_ms >= 1 keeps the argument finite at
 * every supported rate. */
double release_coefficient(double release_ms, int32_t sample_rate) {
    const double tau_frames = release_ms * 0.001 * static_cast<double>(sample_rate);
    return 1.0 - std::exp(-1.0 / tau_frames);
}

}  // namespace

struct dg_limiter {
    int32_t sample_rate = 0;
    int32_t channel_count = 0;
    int32_t linked = 1;
    int32_t true_peak_detection = 0;
    double ceiling_dbfs = 0.0;
    double ceiling_linear = 1.0;
    double release_coefficient = 1.0;
    uint32_t latency_frames = 0;
    uint32_t window_size = 1;

    float* delay = nullptr;      /* channel_count * latency_frames, or NULL when latency is 0 */
    double* required = nullptr;  /* channel_count * window_size, ring by deque slot */
    uint64_t* deque = nullptr;   /* channel_count * window_size, absolute sample indices */

    uint32_t delay_pos = 0;
    uint64_t window_index = 0;
    uint32_t deque_head[kMaxChannels] = {};
    uint32_t deque_count[kMaxChannels] = {};

    double gain[kMaxChannels] = {1.0, 1.0};
    double coefficients[dreamguard::kPhases][kGuardHistory] = {};
    double bound_coefficients[dreamguard::kPhases][kGuardHistory] = {};
    double history[kMaxChannels][kGuardHistory] = {};
    int history_pos = 0;

    double block_input_peak = 0.0;
    double block_output_peak = 0.0;
    double block_input_true_peak = 0.0;
    uint64_t frames = 0;
    uint64_t limited_samples = 0;
    uint64_t over_ceiling_samples = 0;
    uint64_t non_finite_samples = 0;
};

namespace {

void clear_delay_and_window(dg_limiter* limiter) {
    if (limiter->delay != nullptr) {
        const std::size_t count =
            static_cast<std::size_t>(limiter->channel_count) *
            static_cast<std::size_t>(limiter->latency_frames);
        for (std::size_t i = 0; i < count; ++i) {
            limiter->delay[i] = 0.0F;
        }
    }
    for (int32_t ch = 0; ch < kMaxChannels; ++ch) {
        limiter->gain[ch] = 1.0;
        limiter->deque_head[ch] = 0;
        limiter->deque_count[ch] = 0;
    }
    limiter->delay_pos = 0;
    limiter->window_index = 0;
    limiter->history_pos = 0;
    for (auto& channel : limiter->history) {
        for (double& value : channel) value = 0.0;
    }
}

void reset_state(dg_limiter* limiter) {
    limiter->block_input_peak = 0.0;
    limiter->block_output_peak = 0.0;
    limiter->block_input_true_peak = 0.0;
    limiter->frames = 0;
    limiter->limited_samples = 0;
    limiter->over_ceiling_samples = 0;
    limiter->non_finite_samples = 0;
    clear_delay_and_window(limiter);

}

/* Adds one required gain to the sliding-minimum deque of one channel.
 *
 * The deque holds absolute sample indices in increasing order of required
 * gain, so the front is the minimum over the last window_size samples. Each
 * index enters and leaves at most once, which keeps the per-frame cost O(1)
 * and the process path allocation-free. */
void push_required(dg_limiter* limiter, int32_t channel, double required_gain) {
    const std::size_t base =
        static_cast<std::size_t>(channel) * static_cast<std::size_t>(limiter->window_size);
    const uint32_t capacity = limiter->window_size;
    uint32_t head = limiter->deque_head[channel];
    uint32_t count = limiter->deque_count[channel];

    /* Drop indices that are older than the window ending at the current
     * sample. This runs before insertion: if the deque is full, the new index
     * makes exactly one slot free, so tail never aliases the front. */
    while (count > 0) {
        const uint64_t front_index = limiter->deque[base + head];
        if (front_index + static_cast<uint64_t>(limiter->window_size) >
            limiter->window_index) {
            break;
        }
        head = (head + 1U) % capacity;
        --count;
    }

    /* Strictly increasing values: pop every back element >= the new value. */
    while (count > 0) {
        const uint32_t back = (head + count - 1U) % capacity;
        if (limiter->required[base + back] < required_gain) {
            break;
        }
        --count;
    }

    const uint32_t tail = (head + count) % capacity;
    limiter->required[base + tail] = required_gain;
    limiter->deque[base + tail] = limiter->window_index;
    ++count;

    limiter->deque_head[channel] = head;
    limiter->deque_count[channel] = count;
}

double window_minimum(const dg_limiter* limiter, int32_t channel) {
    const std::size_t base =
        static_cast<std::size_t>(channel) * static_cast<std::size_t>(limiter->window_size);
    return limiter->required[base + limiter->deque_head[channel]];
}

}  // namespace

extern "C" {

dg_status dg_limiter_create(const dg_limiter_config* config, dg_limiter** out_limiter) {
    if (out_limiter == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    *out_limiter = nullptr;
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
    if (!in_range(config->ceiling_dbfs, DG_LIMITER_CEILING_DBFS_MIN,
                  DG_LIMITER_CEILING_DBFS_MAX)) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (!in_range(config->lookahead_ms, DG_LIMITER_LOOKAHEAD_MS_MIN,
                  DG_LIMITER_LOOKAHEAD_MS_MAX)) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (!in_range(config->release_ms, DG_LIMITER_RELEASE_MS_MIN,
                  DG_LIMITER_RELEASE_MS_MAX)) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (config->link_stereo != 0 && config->link_stereo != 1) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (config->true_peak_detection != 0 && config->true_peak_detection != 1) {
        return DG_ERR_INVALID_ARGUMENT;
    }

    dg_limiter* limiter = new (std::nothrow) dg_limiter{};
    if (limiter == nullptr) {
        return DG_ERR_OUT_OF_MEMORY;
    }

    limiter->sample_rate = config->sample_rate;
    limiter->channel_count = config->channel_count;
    limiter->linked = config->link_stereo;
    limiter->true_peak_detection = config->true_peak_detection;
    limiter->ceiling_dbfs = config->ceiling_dbfs;
    limiter->ceiling_linear = std::pow(10.0, config->ceiling_dbfs / 20.0);
    limiter->release_coefficient =
        release_coefficient(config->release_ms, config->sample_rate);

    const double latency =
        config->lookahead_ms * 0.001 * static_cast<double>(config->sample_rate);
    /* ceil() is intentional: the delay must cover the requested time, never
     * round it down. 20 ms at 192 kHz is 3840 frames. */
    limiter->latency_frames = static_cast<uint32_t>(std::ceil(latency));
    if (limiter->true_peak_detection != 0) {
        // Every interpolation window containing a sample must be known before
        // that sample leaves. Longer support also protects HF/noise transients
        // that the short measurement filter underestimates against a reference.
        limiter->latency_frames = std::max(limiter->latency_frames,
            static_cast<uint32_t>(DG_LIMITER_TRUE_PEAK_CONTEXT));
        dreamguard::build_reconstruction_filter(limiter->coefficients);
        double short_filter[dreamguard::kPhases][dreamguard::kHistory] = {};
        dreamguard::build_reconstruction_filter(short_filter);
        constexpr int shift = (kGuardHistory - dreamguard::kHistory) / 2;
        for (int phase = 0; phase < dreamguard::kPhases; ++phase) {
            for (int tap = 0; tap < kGuardHistory; ++tap) {
                const double short_weight = tap >= shift && tap < shift + dreamguard::kHistory
                    ? std::fabs(short_filter[phase][tap - shift]) : 0.0;
                limiter->bound_coefficients[phase][tap] = std::max(short_weight,
                    std::fabs(limiter->coefficients[phase][tap]));
            }
        }
    }
    limiter->window_size = limiter->latency_frames + 1U;

    const std::size_t channels = static_cast<std::size_t>(limiter->channel_count);
    const std::size_t window = static_cast<std::size_t>(limiter->window_size);
    limiter->required = new (std::nothrow) double[channels * window];
    limiter->deque = new (std::nothrow) uint64_t[channels * window];
    if (limiter->latency_frames > 0) {
        limiter->delay = new (std::nothrow)
            float[channels * static_cast<std::size_t>(limiter->latency_frames)];
    }
    if (limiter->required == nullptr || limiter->deque == nullptr ||
        (limiter->latency_frames > 0 && limiter->delay == nullptr)) {
        delete[] limiter->required;
        delete[] limiter->deque;
        delete[] limiter->delay;
        delete limiter;
        return DG_ERR_OUT_OF_MEMORY;
    }

    reset_state(limiter);
    *out_limiter = limiter;
    return DG_OK;
}

dg_status dg_limiter_destroy(dg_limiter* limiter) {
    if (limiter == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    delete[] limiter->required;
    delete[] limiter->deque;
    delete[] limiter->delay;
    delete limiter;
    return DG_OK;
}

dg_status dg_limiter_reset(dg_limiter* limiter) {
    if (limiter == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    reset_state(limiter);
    return DG_OK;
}

dg_status dg_limiter_process(dg_limiter* limiter, dg_pcm_view input,
                             dg_pcm_buffer output) {
    if (limiter == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    if (input.frame_count == 0) {
        return DG_OK;
    }
    if (input.channel_count != limiter->channel_count ||
        output.channel_count != limiter->channel_count) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (input.frame_count != output.frame_count) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (input.samples == nullptr || output.samples == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (input.samples != output.samples) {
        /* Partial overlap would make the interleaved read/write order
         * undefined; only the exact in-place case is supported. */
        const std::uintptr_t in_begin =
            reinterpret_cast<std::uintptr_t>(input.samples);
        const std::uintptr_t out_begin =
            reinterpret_cast<std::uintptr_t>(output.samples);
        const std::uintptr_t bytes =
            static_cast<std::uintptr_t>(input.frame_count) *
            static_cast<std::uintptr_t>(limiter->channel_count) * sizeof(float);
        if (in_begin < out_begin + bytes && out_begin < in_begin + bytes) {
            return DG_ERR_INVALID_ARGUMENT;
        }
    }

    const int32_t channels = limiter->channel_count;
    const uint32_t frame_count = input.frame_count;
    const uint32_t latency = limiter->latency_frames;
    const float* source = input.samples;
    float* destination = output.samples;

    double input_peak = 0.0;
    double output_peak = 0.0;
    limiter->block_input_true_peak = 0.0;
    for (uint32_t frame = 0; frame < frame_count; ++frame) {
        const std::size_t base =
            static_cast<std::size_t>(frame) * static_cast<std::size_t>(channels);
        double values[kMaxChannels] = {0.0, 0.0};
        double magnitude[kMaxChannels] = {0.0, 0.0};
        double detector_magnitude[kMaxChannels] = {0.0, 0.0};
        for (int32_t ch = 0; ch < channels; ++ch) {
            const double sample =
                static_cast<double>(source[base + static_cast<std::size_t>(ch)]);
            if (!std::isfinite(sample)) {
                ++limiter->non_finite_samples;
                continue; /* values[ch] stays 0.0, magnitude[ch] stays 0.0 */
            }
            values[ch] = sample;
            magnitude[ch] = std::fabs(sample);
            detector_magnitude[ch] = magnitude[ch];
            if (magnitude[ch] > input_peak) {
                input_peak = magnitude[ch];
            }
        }

        if (limiter->true_peak_detection != 0) {
            for (int32_t ch = 0; ch < channels; ++ch) {
                limiter->history[ch][limiter->history_pos] = values[ch];
                for (int phase = 0; phase < dreamguard::kPhases; ++phase) {
                    double reconstructed = 0.0;
                    double bound = 0.0;
                    for (int tap = 0; tap < kGuardHistory; ++tap) {
                        int index = limiter->history_pos - tap;
                        if (index < 0) index += kGuardHistory;
                        const double term = limiter->coefficients[phase][tap] *
                                            limiter->history[ch][index];
                        reconstructed += term;
                        bound += limiter->bound_coefficients[phase][tap] *
                                 std::fabs(limiter->history[ch][index]);
                    }
                    limiter->block_input_true_peak = std::max(
                        limiter->block_input_true_peak, std::fabs(reconstructed));
                    detector_magnitude[ch] = std::max(detector_magnitude[ch], bound);
                }
            }
            limiter->history_pos = (limiter->history_pos + 1) % kGuardHistory;
        }

        /* Required gain for this frame. Linked channels use the loudest one,
         * so the stereo image is preserved; unlinked channels decide alone. */
        double required[kMaxChannels] = {1.0, 1.0};
        if (limiter->linked != 0) {
            double peak = detector_magnitude[0];
            if (channels == 2 && detector_magnitude[1] > peak) {
                peak = detector_magnitude[1];
            }
            const double shared =
                peak > limiter->ceiling_linear ? limiter->ceiling_linear / peak : 1.0;
            for (int32_t ch = 0; ch < channels; ++ch) {
                required[ch] = shared;
            }
        } else {
            for (int32_t ch = 0; ch < channels; ++ch) {
                required[ch] = detector_magnitude[ch] > limiter->ceiling_linear
                                   ? limiter->ceiling_linear / detector_magnitude[ch]
                                   : 1.0;
            }
        }

        for (int32_t ch = 0; ch < channels; ++ch) {
            if (required[ch] < 1.0) {
                ++limiter->limited_samples;
            }
            push_required(limiter, ch, required[ch]);
            const double minimum = window_minimum(limiter, ch);
            double gain = limiter->gain[ch];
            if (minimum < gain) {
                /* Instant attack. Going below the window minimum is harmless
                 * (more attenuation) and is corrected by the release below. */
                gain = minimum;
            } else {
                gain += (minimum - gain) * limiter->release_coefficient;
                if (gain > minimum) {
                    gain = minimum;
                }
            }
            limiter->gain[ch] = gain;
        }

        for (int32_t ch = 0; ch < channels; ++ch) {
            double delayed = values[ch];
            if (latency > 0) {
                const std::size_t delay_base =
                    static_cast<std::size_t>(ch) * static_cast<std::size_t>(latency);
                delayed = static_cast<double>(limiter->delay[delay_base + limiter->delay_pos]);
                limiter->delay[delay_base + limiter->delay_pos] = static_cast<float>(values[ch]);
            }
            const float written =
                static_cast<float>(delayed * limiter->gain[ch]);
            const double written_magnitude = std::fabs(static_cast<double>(written));
            if (written_magnitude > output_peak) {
                output_peak = written_magnitude;
            }
            if (written_magnitude >
                limiter->ceiling_linear * (1.0 + 1e-6)) {
                ++limiter->over_ceiling_samples;
            }
            destination[base + static_cast<std::size_t>(ch)] = written;
        }

        ++limiter->window_index;
        if (latency > 0) {
            ++limiter->delay_pos;
            if (limiter->delay_pos == latency) {
                limiter->delay_pos = 0;
            }
        }
    }

    limiter->block_input_peak = input_peak;
    limiter->block_output_peak = output_peak;
    limiter->frames += frame_count;
    return DG_OK;
}

dg_status dg_limiter_flush(dg_limiter* limiter, dg_pcm_buffer output,
                           uint32_t* out_frames) {
    if (limiter == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    if (out_frames == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    *out_frames = 0;
    if (output.channel_count != limiter->channel_count) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    const uint32_t latency = limiter->latency_frames;
    if (latency == 0) {
        /* No tail to emit, but flush still ends the stream. */
        clear_delay_and_window(limiter);
        return DG_OK;
    }
    if (output.samples == nullptr || output.frame_count < latency) {
        return DG_ERR_INVALID_ARGUMENT;
    }

    const int32_t channels = limiter->channel_count;
    if (limiter->true_peak_detection != 0) {
        // Complete the detector's right-hand context as well as the audio delay.
        // Processing zeros advances exactly the same envelope as a continued
        // stream. Preserve the public input counters and last-block meters.
        const auto saved_frames = limiter->frames;
        const auto saved_limited = limiter->limited_samples;
        const auto saved_nonfinite = limiter->non_finite_samples;
        const auto saved_over = limiter->over_ceiling_samples;
        const double saved_input = limiter->block_input_peak;
        const double saved_output = limiter->block_output_peak;
        const double saved_true = limiter->block_input_true_peak;
        const float zeros[kMaxChannels] = {};
        for (uint32_t frame = 0; frame < latency; ++frame) {
            dg_limiter_process(limiter, {zeros, 1, channels},
                               {output.samples + frame * channels, 1, channels});
        }
        limiter->frames = saved_frames;
        limiter->limited_samples = saved_limited;
        limiter->non_finite_samples = saved_nonfinite;
        limiter->over_ceiling_samples = saved_over;
        limiter->block_input_peak = saved_input;
        limiter->block_output_peak = saved_output;
        limiter->block_input_true_peak = saved_true;
    } else {
        for (uint32_t frame = 0; frame < latency; ++frame) {
            const std::size_t base =
                static_cast<std::size_t>(frame) * static_cast<std::size_t>(channels);
            for (int32_t ch = 0; ch < channels; ++ch) {
                const std::size_t delay_base =
                    static_cast<std::size_t>(ch) * static_cast<std::size_t>(latency);
                const float delayed = limiter->delay[delay_base + limiter->delay_pos];
                output.samples[base + static_cast<std::size_t>(ch)] =
                    static_cast<float>(static_cast<double>(delayed) * limiter->gain[ch]);
            }
            ++limiter->delay_pos;
            if (limiter->delay_pos == latency) {
                limiter->delay_pos = 0;
            }
        }
    }
    *out_frames = latency;

    /* The stream is finished: return the envelope and delay line to their
     * initial state, but keep the counters and the last block peaks. */
    clear_delay_and_window(limiter);
    return DG_OK;
}

dg_status dg_limiter_snapshot(const dg_limiter* limiter,
                              dg_limiter_values* out_values) {
    if (out_values == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (limiter == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }

    dg_limiter_values values{};
    double gain = limiter->gain[0];
    for (int32_t ch = 1; ch < limiter->channel_count; ++ch) {
        if (limiter->gain[ch] < gain) {
            gain = limiter->gain[ch];
        }
    }
    values.ceiling_dbfs = limiter->ceiling_dbfs;
    values.current_gain_db = level_to_db(gain);
    values.input_peak_db = level_to_db(limiter->block_input_peak);
    values.output_peak_db = level_to_db(limiter->block_output_peak);
    values.frames = limiter->frames;
    values.limited_samples = limiter->limited_samples;
    values.over_ceiling_samples = limiter->over_ceiling_samples;
    values.non_finite_samples = limiter->non_finite_samples;
    values.latency_frames = limiter->latency_frames;
    values.linked = limiter->linked;
    values.true_peak_detection = limiter->true_peak_detection;
    values.input_true_peak_db = level_to_db(limiter->block_input_true_peak);
    *out_values = values;
    return DG_OK;
}

uint32_t dg_limiter_latency_frames(const dg_limiter* limiter) {
    return limiter == nullptr ? 0U : limiter->latency_frames;
}

}  // extern "C"
