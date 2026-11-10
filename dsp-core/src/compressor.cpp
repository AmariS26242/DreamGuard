#include "dreamguard/compressor.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>

namespace {

constexpr int32_t kMaxChannels = DG_PCM_CHANNELS_MAX;
/* ln(10) / 20: converts dB to a natural-log exponent, so gain = exp(db * k). */
constexpr double kDbToNeper = 0.11512925464970229;

bool in_range(double value, double minimum, double maximum) {
    return value >= minimum && value <= maximum;
}

/* Detector level of a sample peak, in dBFS. Silence is clamped to
 * DG_COMPRESSOR_LEVEL_FLOOR so the curve never sees -INFINITY. */
double level_to_db(double peak) {
    const double clamped = peak > DG_COMPRESSOR_LEVEL_FLOOR ? peak : DG_COMPRESSOR_LEVEL_FLOOR;
    return 20.0 * std::log10(clamped);
}

/* The static (zero-attack) curve. level_db is the detector level, the result is
 * a gain REDUCTION in dB and is always <= 0. */
double static_gain_db(double threshold_db, double ratio, double knee_db, double level_db) {
    const double x = level_db - threshold_db;
    const double slope = 1.0 / ratio - 1.0;
    if (knee_db <= 0.0) {
        /* Hard knee: exactly 0 at the threshold, slope * x above it. */
        return x > 0.0 ? slope * x : 0.0;
    }
    const double half = knee_db * 0.5;
    if (x <= -half) {
        return 0.0;
    }
    if (x >= half) {
        return slope * x;
    }
    const double t = x + half;
    return slope * t * t / (2.0 * knee_db);
}

/* One-pole ballistics: 1 - exp(-1 / (tau * fs)). tau >= 0.1 ms keeps the
 * argument finite at every supported rate, including 8 kHz. */
double ballistics_coefficient(double time_ms, int32_t sample_rate) {
    const double tau_frames = time_ms * 0.001 * static_cast<double>(sample_rate);
    return 1.0 - std::exp(-1.0 / tau_frames);
}

}  // namespace

struct dg_compressor {
    int32_t sample_rate = 0;
    int32_t channel_count = 0;
    double threshold_db = 0.0;
    double ratio = 1.0;
    double knee_db = 0.0;
    double makeup_gain_db = 0.0;
    int32_t linked = 1;
    double attack_coefficient = 1.0;
    double release_coefficient = 1.0;

    /* Smoothed gain reduction per channel, <= 0 dB. Linked channels keep both
     * entries equal so a snapshot can report either one. */
    double gain_reduction_db[kMaxChannels] = {};
    double static_gain_reduction_db = 0.0;

    double block_input_peak = 0.0;
    double block_output_peak = 0.0;

    uint64_t frames = 0;
    uint64_t clip_samples = 0;
    uint64_t non_finite_samples = 0;
};

namespace {

void reset_state(dg_compressor* compressor) {
    for (int32_t ch = 0; ch < kMaxChannels; ++ch) {
        compressor->gain_reduction_db[ch] = 0.0;
    }
    compressor->static_gain_reduction_db = 0.0;
    compressor->block_input_peak = 0.0;
    compressor->block_output_peak = 0.0;
    compressor->frames = 0;
    compressor->clip_samples = 0;
    compressor->non_finite_samples = 0;
}

/* Moves one channel's gain reduction toward the target with the attack
 * coefficient when more reduction is required and the release coefficient when
 * it is being given back. */
void smooth_toward(dg_compressor* compressor, int32_t channel, double target_db) {
    const double coefficient = target_db < compressor->gain_reduction_db[channel]
                                   ? compressor->attack_coefficient
                                   : compressor->release_coefficient;
    compressor->gain_reduction_db[channel] +=
        (target_db - compressor->gain_reduction_db[channel]) * coefficient;
}

}  // namespace

extern "C" {

dg_status dg_compressor_create(const dg_compressor_config* config,
                               dg_compressor** out_compressor) {
    if (out_compressor == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    *out_compressor = nullptr;
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
    if (!in_range(config->threshold_db, DG_COMPRESSOR_THRESHOLD_DB_MIN,
                  DG_COMPRESSOR_THRESHOLD_DB_MAX)) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (!in_range(config->ratio, DG_COMPRESSOR_RATIO_MIN, DG_COMPRESSOR_RATIO_MAX)) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (!in_range(config->knee_db, DG_COMPRESSOR_KNEE_DB_MIN, DG_COMPRESSOR_KNEE_DB_MAX)) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (!in_range(config->attack_ms, DG_COMPRESSOR_ATTACK_MS_MIN,
                  DG_COMPRESSOR_ATTACK_MS_MAX)) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (!in_range(config->release_ms, DG_COMPRESSOR_RELEASE_MS_MIN,
                  DG_COMPRESSOR_RELEASE_MS_MAX)) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (!in_range(config->makeup_gain_db, DG_COMPRESSOR_MAKEUP_DB_MIN,
                  DG_COMPRESSOR_MAKEUP_DB_MAX)) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (config->link_stereo != 0 && config->link_stereo != 1) {
        return DG_ERR_INVALID_ARGUMENT;
    }

    dg_compressor* compressor = new (std::nothrow) dg_compressor{};
    if (compressor == nullptr) {
        return DG_ERR_OUT_OF_MEMORY;
    }
    compressor->sample_rate = config->sample_rate;
    compressor->channel_count = config->channel_count;
    compressor->threshold_db = config->threshold_db;
    compressor->ratio = config->ratio;
    compressor->knee_db = config->knee_db;
    compressor->makeup_gain_db = config->makeup_gain_db;
    compressor->linked = config->link_stereo;
    compressor->attack_coefficient =
        ballistics_coefficient(config->attack_ms, config->sample_rate);
    compressor->release_coefficient =
        ballistics_coefficient(config->release_ms, config->sample_rate);
    reset_state(compressor);
    *out_compressor = compressor;
    return DG_OK;
}

dg_status dg_compressor_destroy(dg_compressor* compressor) {
    if (compressor == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    delete compressor;
    return DG_OK;
}

dg_status dg_compressor_reset(dg_compressor* compressor) {
    if (compressor == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    reset_state(compressor);
    return DG_OK;
}

dg_status dg_compressor_process(dg_compressor* compressor, dg_pcm_view input,
                                dg_pcm_buffer output) {
    if (compressor == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    if (input.frame_count == 0) {
        return DG_OK;
    }
    if (input.channel_count != compressor->channel_count ||
        output.channel_count != compressor->channel_count) {
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
            static_cast<std::uintptr_t>(compressor->channel_count) * sizeof(float);
        if (in_begin < out_begin + bytes && out_begin < in_begin + bytes) {
            return DG_ERR_INVALID_ARGUMENT;
        }
    }

    const int32_t channels = compressor->channel_count;
    const uint32_t frame_count = input.frame_count;
    const float* source = input.samples;
    float* destination = output.samples;

    double input_peak = 0.0;
    double output_peak = 0.0;
    for (uint32_t frame = 0; frame < frame_count; ++frame) {
        const std::size_t base =
            static_cast<std::size_t>(frame) * static_cast<std::size_t>(channels);
        double values[kMaxChannels] = {0.0, 0.0};
        double magnitude[kMaxChannels] = {0.0, 0.0};
        for (int32_t ch = 0; ch < channels; ++ch) {
            const double sample = static_cast<double>(source[base + static_cast<std::size_t>(ch)]);
            if (!std::isfinite(sample)) {
                ++compressor->non_finite_samples;
                continue; /* values[ch] stays 0.0, magnitude[ch] stays 0.0 */
            }
            values[ch] = sample;
            magnitude[ch] = std::fabs(sample);
            if (magnitude[ch] > input_peak) {
                input_peak = magnitude[ch];
            }
        }

        /* Detector and gain computer. Linked channels share one target built
         * from the loudest channel, so the stereo image survives. */
        double target_db = 0.0;
        if (compressor->linked != 0) {
            double peak = magnitude[0];
            if (channels == 2 && magnitude[1] > peak) {
                peak = magnitude[1];
            }
            target_db = static_gain_db(compressor->threshold_db, compressor->ratio,
                                       compressor->knee_db, level_to_db(peak));
            compressor->static_gain_reduction_db = target_db;
            for (int32_t ch = 0; ch < channels; ++ch) {
                smooth_toward(compressor, ch, target_db);
            }
        } else {
            for (int32_t ch = 0; ch < channels; ++ch) {
                target_db = static_gain_db(compressor->threshold_db, compressor->ratio,
                                           compressor->knee_db,
                                           level_to_db(magnitude[ch]));
                if (ch == 0) {
                    compressor->static_gain_reduction_db = target_db;
                }
                smooth_toward(compressor, ch, target_db);
            }
        }

        for (int32_t ch = 0; ch < channels; ++ch) {
            const double gain_db =
                compressor->gain_reduction_db[ch] + compressor->makeup_gain_db;
            const double written = values[ch] * std::exp(gain_db * kDbToNeper);
            const double written_magnitude = std::fabs(written);
            if (written_magnitude > output_peak) {
                output_peak = written_magnitude;
            }
            if (std::isfinite(written) && written_magnitude > 1.0) {
                ++compressor->clip_samples;
            }
            destination[base + static_cast<std::size_t>(ch)] =
                static_cast<float>(written);
        }
    }

    compressor->block_input_peak = input_peak;
    compressor->block_output_peak = output_peak;
    compressor->frames += frame_count;
    return DG_OK;
}

dg_status dg_compressor_snapshot(const dg_compressor* compressor,
                                 dg_compressor_values* out_values) {
    if (out_values == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (compressor == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }

    dg_compressor_values values{};
    /* Unlinked channels can differ; report the one that is reducing more. */
    double gain_reduction = compressor->gain_reduction_db[0];
    for (int32_t ch = 1; ch < compressor->channel_count; ++ch) {
        if (compressor->gain_reduction_db[ch] < gain_reduction) {
            gain_reduction = compressor->gain_reduction_db[ch];
        }
    }
    values.gain_reduction_db = gain_reduction;
    values.static_gain_reduction_db = compressor->static_gain_reduction_db;
    values.input_peak_db = level_to_db(compressor->block_input_peak);
    values.output_peak_db = level_to_db(compressor->block_output_peak);
    values.frames = compressor->frames;
    values.clip_samples = compressor->clip_samples;
    values.non_finite_samples = compressor->non_finite_samples;
    values.linked = compressor->linked;
    *out_values = values;
    return DG_OK;
}

dg_status dg_compressor_static_gain_db(const dg_compressor_config* config,
                                       double level_db, double* out_gain_db) {
    if (out_gain_db == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    *out_gain_db = 0.0;
    if (config == nullptr || !std::isfinite(level_db)) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (config->ratio < DG_COMPRESSOR_RATIO_MIN ||
        config->ratio > DG_COMPRESSOR_RATIO_MAX) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (config->knee_db < DG_COMPRESSOR_KNEE_DB_MIN ||
        config->knee_db > DG_COMPRESSOR_KNEE_DB_MAX) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (!std::isfinite(config->threshold_db)) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    *out_gain_db = static_gain_db(config->threshold_db, config->ratio, config->knee_db,
                                  level_db);
    return DG_OK;
}

}  // extern "C"
