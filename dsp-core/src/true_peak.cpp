#include "dreamguard/true_peak.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

namespace {

constexpr int32_t kMaxChannels = DG_PCM_CHANNELS_MAX;
constexpr int32_t kOversampling = DG_TRUE_PEAK_OVERSAMPLING;
constexpr int32_t kTapsPerPhase = 16;
constexpr int32_t kFilterTaps = kOversampling * kTapsPerPhase + 1; /* 65 */
constexpr int32_t kFilterCenter = kFilterTaps / 2;                 /* 32 */
constexpr int32_t kHistory = kTapsPerPhase + 1;                    /* 17 */
constexpr double kPi = 3.14159265358979323846;

bool in_range(int32_t value, int32_t minimum, int32_t maximum) {
    return value >= minimum && value <= maximum;
}

/* Peak magnitude to dBFS with the same floor convention as the limiter and the
 * meter: silence is a finite -240 dBFS, never -INFINITY. */
double level_to_db(double peak) {
    const double clamped = peak > DG_TRUE_PEAK_LEVEL_FLOOR ? peak : DG_TRUE_PEAK_LEVEL_FLOOR;
    return 20.0 * std::log10(clamped);
}

/* Builds the 4x polyphase interpolator.
 *
 * h[j] = sinc((j - centre) / 4) * blackman(j), j = 0..64. Its cutoff sits at the
 * input Nyquist frequency, one quarter of the upsampled Nyquist, so the imaging
 * copies produced by zero-stuffing fall in the stopband. The windowed-sinc has
 * a DC gain of 4; each polyphase branch is normalised to 1 so a constant input
 * is reproduced exactly.
 *
 * Branch 0 is forced to a pure delay (a single centre tap). That makes the
 * original sample positions part of the true-peak maximum once the 8-frame
 * centre delay has elapsed; a stream shorter than that can under-report the
 * tail (see true_peak.h).
 */
void build_filter(double coefficients[kOversampling][kHistory]) {
    double filter[kFilterTaps] = {};
    const double denominator = static_cast<double>(kFilterTaps - 1);
    for (int32_t j = 0; j < kFilterTaps; ++j) {
        const double x =
            static_cast<double>(j - kFilterCenter) / static_cast<double>(kOversampling);
        const double sinc = (j == kFilterCenter) ? 1.0 : std::sin(kPi * x) / (kPi * x);
        const double angle = 2.0 * kPi * static_cast<double>(j) / denominator;
        const double window = 0.42 - 0.5 * std::cos(angle) + 0.08 * std::cos(2.0 * angle);
        filter[j] = sinc * window;
    }

    for (int32_t phase = 0; phase < kOversampling; ++phase) {
        double sum = 0.0;
        for (int32_t tap = 0; tap < kHistory; ++tap) {
            const int32_t j = phase + kOversampling * tap;
            const double value = j < kFilterTaps ? filter[j] : 0.0;
            coefficients[phase][tap] = value;
            sum += value;
        }
        if (phase == 0) {
            /* Exact delayed copy of the input at the sample positions. */
            for (int32_t tap = 0; tap < kHistory; ++tap) {
                coefficients[0][tap] = 0.0;
            }
            coefficients[0][kFilterCenter / kOversampling] = 1.0;
        } else if (sum != 0.0) {
            for (int32_t tap = 0; tap < kHistory; ++tap) {
                coefficients[phase][tap] /= sum;
            }
        }
    }
}

}  // namespace

struct dg_true_peak {
    int32_t sample_rate = 0;
    int32_t channel_count = 0;
    int32_t history_pos = 0;
    double coefficients[kOversampling][kHistory] = {};
    double history[kMaxChannels][kHistory] = {};

    double block_sample_peak = 0.0;
    double block_true_peak = 0.0;
    double total_sample_peak = 0.0;
    double total_true_peak = 0.0;
    uint64_t frames = 0;
    uint64_t non_finite_samples = 0;
};

namespace {

void clear_state(dg_true_peak* true_peak) {
    for (int32_t ch = 0; ch < kMaxChannels; ++ch) {
        for (int32_t tap = 0; tap < kHistory; ++tap) {
            true_peak->history[ch][tap] = 0.0;
        }
    }
    true_peak->history_pos = 0;
    true_peak->block_sample_peak = 0.0;
    true_peak->block_true_peak = 0.0;
    true_peak->total_sample_peak = 0.0;
    true_peak->total_true_peak = 0.0;
    true_peak->frames = 0;
    true_peak->non_finite_samples = 0;
}

}  // namespace

extern "C" {

dg_status dg_true_peak_create(const dg_true_peak_config* config,
                              dg_true_peak** out_true_peak) {
    if (out_true_peak == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    *out_true_peak = nullptr;
    if (config == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (!in_range(config->sample_rate, DG_SAMPLE_RATE_MIN, DG_SAMPLE_RATE_MAX)) {
        return DG_ERR_SAMPLE_RATE_OUT_OF_RANGE;
    }
    if (!in_range(config->channel_count, DG_PCM_CHANNELS_MIN, DG_PCM_CHANNELS_MAX)) {
        return DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE;
    }

    dg_true_peak* true_peak = new (std::nothrow) dg_true_peak{};
    if (true_peak == nullptr) {
        return DG_ERR_OUT_OF_MEMORY;
    }
    true_peak->sample_rate = config->sample_rate;
    true_peak->channel_count = config->channel_count;
    build_filter(true_peak->coefficients);
    clear_state(true_peak);
    *out_true_peak = true_peak;
    return DG_OK;
}

dg_status dg_true_peak_destroy(dg_true_peak* true_peak) {
    if (true_peak == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    delete true_peak;
    return DG_OK;
}

dg_status dg_true_peak_reset(dg_true_peak* true_peak) {
    if (true_peak == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    clear_state(true_peak);
    return DG_OK;
}

dg_status dg_true_peak_process(dg_true_peak* true_peak, dg_pcm_view input) {
    if (true_peak == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    if (input.frame_count == 0) {
        return DG_OK;
    }
    if (input.channel_count != true_peak->channel_count) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (input.samples == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }

    const int32_t channels = true_peak->channel_count;
    const uint32_t frame_count = input.frame_count;
    double block_sample_peak = 0.0;
    double block_true_peak = 0.0;

    for (uint32_t frame = 0; frame < frame_count; ++frame) {
        const std::size_t base =
            static_cast<std::size_t>(frame) * static_cast<std::size_t>(channels);
        for (int32_t ch = 0; ch < channels; ++ch) {
            double sample = static_cast<double>(input.samples[base + static_cast<std::size_t>(ch)]);
            if (!std::isfinite(sample)) {
                ++true_peak->non_finite_samples;
                sample = 0.0;
            } else {
                const double magnitude = std::fabs(sample);
                if (magnitude > block_sample_peak) {
                    block_sample_peak = magnitude;
                }
            }
            true_peak->history[ch][true_peak->history_pos] = sample;
        }

        for (int32_t ch = 0; ch < channels; ++ch) {
            for (int32_t phase = 0; phase < kOversampling; ++phase) {
                double interpolated = 0.0;
                for (int32_t tap = 0; tap < kHistory; ++tap) {
                    int32_t index = true_peak->history_pos - tap;
                    if (index < 0) {
                        index += kHistory;
                    }
                    interpolated += true_peak->coefficients[phase][tap] *
                                    true_peak->history[ch][index];
                }
                const double magnitude = std::fabs(interpolated);
                if (magnitude > block_true_peak) {
                    block_true_peak = magnitude;
                }
            }
        }

        ++true_peak->history_pos;
        if (true_peak->history_pos == kHistory) {
            true_peak->history_pos = 0;
        }
    }

    true_peak->block_sample_peak = block_sample_peak;
    true_peak->block_true_peak = block_true_peak;
    if (block_sample_peak > true_peak->total_sample_peak) {
        true_peak->total_sample_peak = block_sample_peak;
    }
    if (block_true_peak > true_peak->total_true_peak) {
        true_peak->total_true_peak = block_true_peak;
    }
    true_peak->frames += frame_count;
    return DG_OK;
}

dg_status dg_true_peak_snapshot(const dg_true_peak* true_peak,
                                dg_true_peak_values* out_values) {
    if (out_values == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (true_peak == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }

    dg_true_peak_values values{};
    values.sample_peak_linear = true_peak->block_sample_peak;
    values.sample_peak_dbfs = level_to_db(true_peak->block_sample_peak);
    values.true_peak_linear = true_peak->block_true_peak;
    values.true_peak_dbfs = level_to_db(true_peak->block_true_peak);
    values.total_sample_peak_linear = true_peak->total_sample_peak;
    values.total_sample_peak_dbfs = level_to_db(true_peak->total_sample_peak);
    values.total_true_peak_linear = true_peak->total_true_peak;
    values.total_true_peak_dbfs = level_to_db(true_peak->total_true_peak);
    values.frames = true_peak->frames;
    values.non_finite_samples = true_peak->non_finite_samples;
    values.channel_count = true_peak->channel_count;
    values.oversampling_factor = kOversampling;
    values.filter_taps = static_cast<uint32_t>(kFilterTaps);
    values.filter_latency_frames = static_cast<uint32_t>(DG_TRUE_PEAK_FILTER_LATENCY);
    *out_values = values;
    return DG_OK;
}

double dg_true_peak_linear_to_dbfs(double linear) {
    if (std::isnan(linear)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double clamped = linear > DG_TRUE_PEAK_LEVEL_FLOOR ? linear : DG_TRUE_PEAK_LEVEL_FLOOR;
    return 20.0 * std::log10(clamped);
}

}  // extern "C"
