/* DreamGuard DSP core - PCM16 <-> compressor bridge (DG-022).
 *
 * See dreamguard/pcm16_processor.h for the contract. This file is deliberately
 * small: conversion, chunking and the optional bit-exact copy. All dynamics
 * behaviour lives in the DG-013 compressor.
 */

#include "dreamguard/pcm16_processor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "dreamguard/compressor.h"
#include "dreamguard/limiter.h"

namespace {

/* 1.0 in the int16 domain is 32767, but the standard mapping is s / 32768, so
 * full-scale negative maps to exactly -1.0 and full-scale positive to
 * 32767/32768. This is the same convention as PCM16 in every other tool. */
constexpr float kPcm16Scale = 32768.0F;
constexpr float kPcm16InverseScale = 1.0F / 32768.0F;

/* Reads a native-endian PCM16 sample without assuming 2-byte alignment: a
 * direct ByteBuffer from Media3 is not guaranteed to be more than byte aligned,
 * and a misaligned int16_t load is undefined behaviour. */
inline int16_t load_sample(const uint8_t* source) {
    int16_t value = 0;
    std::memcpy(&value, source, sizeof(value));
    return value;
}

inline void store_sample(uint8_t* destination, int16_t value) {
    std::memcpy(destination, &value, sizeof(value));
}

/* Rounds half away from zero and saturates. The compressor has no limiter, so
 * a positive makeup gain can drive the signal past full scale; saturating is
 * the only sane PCM16 result and is what the header documents. Non-finite
 * values cannot come out of the compressor, but a zero fallback keeps the
 * conversion total instead of relying on undefined float-to-int behaviour. */
inline int16_t quantize(float sample) {
    if (!std::isfinite(sample)) {
        return 0;
    }
    const float scaled = sample * kPcm16Scale;
    /* Clamp in float space before the integer conversion: converting a float that
     * is outside the int32 range is undefined behaviour, and a positive makeup
     * gain can legitimately produce values far above full scale. The saturation
     * checks below then become belt-and-braces rather than load-bearing. */
    const float clamped = std::clamp(scaled, -kPcm16Scale, kPcm16Scale - 1.0F);
    int32_t value = 0;
    if (clamped >= 0.0F) {
        value = static_cast<int32_t>(clamped + 0.5F);
        if (value > 32767) {
            value = 32767;
        }
    } else {
        value = static_cast<int32_t>(clamped - 0.5F);
        if (value < -32768) {
            value = -32768;
        }
    }
    return static_cast<int16_t>(value);
}

double current_gain_reduction(const dg_compressor* compressor) {
    dg_compressor_values snapshot{};
    if (dg_compressor_snapshot(compressor, &snapshot) != DG_OK) {
        return 0.0;
    }
    return snapshot.gain_reduction_db;
}

void write_values(dg_pcm16_processor_values* out_values, uint32_t frames,
                  uint32_t bytes_written, int32_t bypassed, double gain_reduction_db) {
    if (out_values == nullptr) {
        return;
    }
    out_values->frames = frames;
    out_values->bytes_written = bytes_written;
    out_values->bypassed = bypassed;
    out_values->gain_reduction_db = gain_reduction_db;
}

}  // namespace

struct dg_pcm16_processor {
    dg_compressor* compressor = nullptr;
    dg_limiter* limiter = nullptr;
    dg_lufs* input_lufs = nullptr;
    dg_lufs* output_lufs = nullptr;
    bool pending_tail = false;
    bool observation_needs_reset = false;
    int32_t sample_rate = 0;
    int32_t channel_count = 0;
    uint32_t frame_bytes = 0;
    double makeup_gain_db = 0.0;
    uint64_t last_call_limited_samples = 0;
    int32_t last_bypassed = 0;
    /* One chunk of interleaved floats; the compressor's input view and output
     * buffer both describe it, which is an exact in-place operation. */
    float scratch[static_cast<std::size_t>(DG_PCM16_CHUNK_FRAMES) *
                  static_cast<std::size_t>(DG_PCM_CHANNELS_MAX)] = {};
};

extern "C" {

dg_status dg_pcm16_processor_create(const dg_pcm16_processor_config* config,
                                    dg_pcm16_processor** out_processor) {
    if (out_processor == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    *out_processor = nullptr;
    if (config == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (config->limiter_enabled != 0 && config->limiter_enabled != 1) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (config->channel_count < DG_PCM_CHANNELS_MIN ||
        config->channel_count > DG_PCM_CHANNELS_MAX) {
        return DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE;
    }

    dg_compressor_config compressor_config{};
    compressor_config.sample_rate = config->sample_rate;
    compressor_config.channel_count = config->channel_count;
    compressor_config.threshold_db = config->threshold_db;
    compressor_config.ratio = config->ratio;
    compressor_config.knee_db = config->knee_db;
    compressor_config.attack_ms = config->attack_ms;
    compressor_config.release_ms = config->release_ms;
    compressor_config.link_stereo = config->link_stereo;
    compressor_config.makeup_gain_db = config->makeup_gain_db;

    dg_compressor* compressor = nullptr;
    const dg_status status = dg_compressor_create(&compressor_config, &compressor);
    if (status != DG_OK) {
        return status;
    }

    auto* processor = new (std::nothrow) dg_pcm16_processor();
    if (processor == nullptr) {
        (void)dg_compressor_destroy(compressor);
        return DG_ERR_OUT_OF_MEMORY;
    }

    processor->compressor = compressor;
    processor->sample_rate = config->sample_rate;
    processor->channel_count = config->channel_count;
    processor->frame_bytes =
        static_cast<uint32_t>(config->channel_count) * DG_PCM16_BYTES_PER_SAMPLE;
    processor->makeup_gain_db = config->makeup_gain_db;
    if (config->limiter_enabled != 0) {
        dg_limiter_config safety{};
        safety.sample_rate = config->sample_rate;
        safety.channel_count = config->channel_count;
        safety.ceiling_dbfs = -1.0;
        safety.lookahead_ms = 0.0;
        safety.release_ms = 100.0;
        safety.link_stereo = 1;
        safety.true_peak_detection = 1;
        const auto limiter_status = dg_limiter_create(&safety, &processor->limiter);
        if (limiter_status != DG_OK) {
            dg_pcm16_processor_destroy(processor);
            return limiter_status;
        }
    }
    const dg_lufs_config lufs_config{config->sample_rate, config->channel_count};
    const dg_status in_lufs_status = dg_lufs_create(&lufs_config, &processor->input_lufs);
    if (in_lufs_status != DG_OK) {
        dg_pcm16_processor_destroy(processor);
        return in_lufs_status;
    }
    const dg_status out_lufs_status = dg_lufs_create(&lufs_config, &processor->output_lufs);
    if (out_lufs_status != DG_OK) {
        dg_pcm16_processor_destroy(processor);
        return out_lufs_status;
    }
    *out_processor = processor;
    return DG_OK;
}

dg_status dg_pcm16_processor_destroy(dg_pcm16_processor* processor) {
    if (processor == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    if (processor->compressor != nullptr) (void)dg_compressor_destroy(processor->compressor);
    if (processor->limiter != nullptr) (void)dg_limiter_destroy(processor->limiter);
    if (processor->input_lufs != nullptr) (void)dg_lufs_destroy(processor->input_lufs);
    if (processor->output_lufs != nullptr) (void)dg_lufs_destroy(processor->output_lufs);
    processor->compressor = nullptr;
    processor->limiter = nullptr;
    processor->input_lufs = nullptr;
    processor->output_lufs = nullptr;
    delete processor;
    return DG_OK;
}

dg_status dg_pcm16_processor_reset(dg_pcm16_processor* processor) {
    if (processor == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    processor->pending_tail = false;
    processor->observation_needs_reset = false;
    processor->last_call_limited_samples = 0;
    processor->last_bypassed = 0;
    if (processor->limiter != nullptr) (void)dg_limiter_reset(processor->limiter);
    if (processor->input_lufs != nullptr) (void)dg_lufs_reset(processor->input_lufs);
    if (processor->output_lufs != nullptr) (void)dg_lufs_reset(processor->output_lufs);
    return dg_compressor_reset(processor->compressor);
}

dg_status dg_pcm16_processor_process(dg_pcm16_processor* processor,
                                     const void* input,
                                     uint32_t input_bytes,
                                     void* output,
                                     uint32_t output_capacity,
                                     int32_t bypass,
                                     dg_pcm16_processor_values* out_values) {
    write_values(out_values, 0, 0, 0, 0.0);
    if (processor == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    if (input_bytes == 0) {
        return DG_OK;
    }
    if (input == nullptr || output == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (output_capacity < input_bytes) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    // Delayed PCM must stay frame-aligned. The legacy compressor-only path
    // continues to preserve partial frames verbatim.
    if (bypass == 0 && processor->limiter != nullptr &&
        input_bytes % processor->frame_bytes != 0) return DG_ERR_INVALID_ARGUMENT;

    const auto input_address = reinterpret_cast<std::uintptr_t>(input);
    const auto output_address = reinterpret_cast<std::uintptr_t>(output);
    if (input_address != output_address) {
        const auto input_end = input_address + input_bytes;
        const auto output_end = output_address + output_capacity;
        if (input_address < output_end && output_address < input_end) {
            return DG_ERR_INVALID_ARGUMENT;
        }
    }

    const auto* source = static_cast<const std::uint8_t*>(input);
    auto* destination = static_cast<std::uint8_t*>(output);
    const uint32_t frames = input_bytes / processor->frame_bytes;
    const uint32_t processed_bytes = frames * processor->frame_bytes;

    if (bypass != 0) {
        std::memmove(destination, source, input_bytes);
        processor->last_call_limited_samples = 0;
        processor->last_bypassed = 1;
        processor->observation_needs_reset = true;
        write_values(out_values, frames, input_bytes, 1, 0.0);
        return DG_OK;
    }

    if (frames > 0 && processor->observation_needs_reset) {
        (void)dg_lufs_reset(processor->input_lufs);
        (void)dg_lufs_reset(processor->output_lufs);
        processor->observation_needs_reset = false;
    }
    uint64_t limited_before = 0;
    if (processor->limiter != nullptr) {
        dg_limiter_values before_values{};
        if (dg_limiter_snapshot(processor->limiter, &before_values) == DG_OK) {
            limited_before = before_values.limited_samples;
        }
    }

    uint32_t frame_index = 0;
    while (frame_index < frames) {
        const uint32_t chunk_frames = std::min(
            frames - frame_index, static_cast<uint32_t>(DG_PCM16_CHUNK_FRAMES));
        const std::size_t samples =
            static_cast<std::size_t>(chunk_frames) *
            static_cast<std::size_t>(processor->channel_count);
        const std::size_t byte_offset =
            static_cast<std::size_t>(frame_index) * processor->frame_bytes;

        for (std::size_t sample = 0; sample < samples; ++sample) {
            processor->scratch[sample] =
                static_cast<float>(load_sample(source + byte_offset +
                                               sample * DG_PCM16_BYTES_PER_SAMPLE)) *
                kPcm16InverseScale;
        }

        const dg_pcm_view input_view{processor->scratch, chunk_frames,
                                     processor->channel_count};
        /* Writable output descriptor: dg_compressor_process takes dg_pcm_buffer for
         * its output, the same type the limiter and the PCM helpers use. */
        dg_pcm_buffer output_view{processor->scratch, chunk_frames,
                                  processor->channel_count};
        const dg_status status =
            dg_compressor_process(processor->compressor, input_view, output_view);
        if (status != DG_OK) {
            return status;
        }
        const dg_status in_obs_status =
            dg_lufs_process(processor->input_lufs, input_view);
        if (in_obs_status != DG_OK) {
            return in_obs_status;
        }
        if (processor->limiter != nullptr) {
            const auto limiter_status = dg_limiter_process(processor->limiter, input_view, output_view);
            if (limiter_status != DG_OK) return limiter_status;
            processor->pending_tail = true;
        }
        const dg_status out_obs_status =
            dg_lufs_process(processor->output_lufs, input_view);
        if (out_obs_status != DG_OK) {
            return out_obs_status;
        }

        for (std::size_t sample = 0; sample < samples; ++sample) {
            store_sample(destination + byte_offset +
                             sample * DG_PCM16_BYTES_PER_SAMPLE,
                         quantize(processor->scratch[sample]));
        }
        frame_index += chunk_frames;
    }

    if (processor->limiter != nullptr) {
        dg_limiter_values after_values{};
        if (dg_limiter_snapshot(processor->limiter, &after_values) == DG_OK &&
            after_values.limited_samples >= limited_before) {
            processor->last_call_limited_samples =
                after_values.limited_samples - limited_before;
        } else {
            processor->last_call_limited_samples = 0;
        }
    } else {
        processor->last_call_limited_samples = 0;
    }
    processor->last_bypassed = 0;

    if (processed_bytes < input_bytes) {
        /* A partial frame (for stereo a single sample, or one odd byte for
         * mono) is passed through unchanged so the byte count is preserved. */
        std::memmove(destination + processed_bytes, source + processed_bytes,
                     input_bytes - processed_bytes);
    }

    write_values(out_values, frames, input_bytes, 0,
                 current_gain_reduction(processor->compressor));
    return DG_OK;
}

uint32_t dg_pcm16_processor_latency_frames(const dg_pcm16_processor* processor) {
    return processor == nullptr ? 0 : dg_limiter_latency_frames(processor->limiter);
}

dg_status dg_pcm16_processor_drain(dg_pcm16_processor* processor, void* output,
                                   uint32_t output_capacity, uint32_t* out_bytes) {
    if (out_bytes == nullptr) return DG_ERR_INVALID_ARGUMENT;
    *out_bytes = 0;
    if (processor == nullptr) return DG_ERR_NULL_HANDLE;
    if (!processor->pending_tail) return DG_OK;
    const auto latency = dg_pcm16_processor_latency_frames(processor);
    const auto bytes = latency * processor->frame_bytes;
    if (output == nullptr || output_capacity < bytes) return DG_ERR_INVALID_ARGUMENT;
    uint32_t frames = 0;
    const auto status = dg_limiter_flush(processor->limiter,
        {processor->scratch, latency, processor->channel_count}, &frames);
    if (status != DG_OK) return status;
    const auto observation_status = dg_lufs_process(processor->output_lufs,
        {processor->scratch, frames, processor->channel_count});
    if (observation_status != DG_OK) return observation_status;
    auto* destination = static_cast<uint8_t*>(output);
    for (uint32_t i = 0; i < frames * processor->channel_count; ++i) {
        store_sample(destination + i * 2, quantize(processor->scratch[i]));
    }
    processor->pending_tail = false;
    // Keep final windows available to snapshot; the next processed segment
    // starts fresh without resetting the compressor or changing audio history.
    processor->observation_needs_reset = true;
    *out_bytes = bytes;
    return DG_OK;
}

dg_status dg_pcm16_processor_snapshot(
    const dg_pcm16_processor* processor,
    dg_pcm16_processor_observation* out_observation) {
    if (out_observation == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (processor == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }

    dg_lufs_window_values in_window{};
    const dg_status in_status =
        dg_lufs_window_snapshot(processor->input_lufs, &in_window);
    if (in_status != DG_OK) {
        return in_status;
    }

    dg_lufs_window_values out_window{};
    const dg_status out_status =
        dg_lufs_window_snapshot(processor->output_lufs, &out_window);
    if (out_status != DG_OK) {
        return out_status;
    }

    dg_compressor_values comp_values{};
    const dg_status comp_status =
        dg_compressor_snapshot(processor->compressor, &comp_values);
    if (comp_status != DG_OK) {
        return comp_status;
    }

    dg_limiter_values lim_values{};
    if (processor->limiter != nullptr) {
        const dg_status lim_status =
            dg_limiter_snapshot(processor->limiter, &lim_values);
        if (lim_status != DG_OK) {
            return lim_status;
        }
    }

    dg_pcm16_processor_observation obs{};
    obs.input_momentary_lufs = in_window.momentary_lufs;
    obs.input_short_term_lufs = in_window.short_term_lufs;
    obs.output_momentary_lufs = out_window.momentary_lufs;
    obs.output_short_term_lufs = out_window.short_term_lufs;
    obs.compressor_gain_reduction_db = comp_values.gain_reduction_db;
    obs.makeup_gain_db = processor->makeup_gain_db;
    obs.loudness_gain_db = 0.0;
    obs.limiter_gain_db =
        processor->limiter != nullptr ? lim_values.current_gain_db : 0.0;
    obs.completed_hops = in_window.completed_hops;
    obs.limiter_limited_samples =
        processor->limiter != nullptr ? lim_values.limited_samples : 0;
    obs.last_call_limited_samples = processor->last_call_limited_samples;
    obs.hop_frames = in_window.hop_frames;
    obs.pending_frames = in_window.pending_frames;
    obs.input_momentary_ready = in_window.momentary_ready;
    obs.input_short_term_ready = in_window.short_term_ready;
    obs.output_momentary_ready = out_window.momentary_ready;
    obs.output_short_term_ready = out_window.short_term_ready;
    obs.limiter_enabled = processor->limiter != nullptr ? 1 : 0;
    obs.limiter_active =
        (processor->last_bypassed == 0 && processor->limiter != nullptr &&
         (processor->last_call_limited_samples > 0 ||
          lim_values.current_gain_db < -1e-4))
            ? 1
            : 0;
    obs.bypassed = processor->last_bypassed;
    *out_observation = obs;
    return DG_OK;
}

}  // extern "C"
