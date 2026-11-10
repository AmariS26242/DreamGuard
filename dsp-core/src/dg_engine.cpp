#include "dreamguard/dsp_core.h"

#include <cstddef>
#include <new>
#include <span>

#include "soundscape.h"

// The C handle is a plain pointer to this object, owned by the caller from
// dg_create until the matching dg_destroy. That is the whole identity model:
// no registry, no lock, no reference count, so the render path costs nothing
// beyond the renderer itself. Reusing a destroyed handle is undefined
// behaviour and is deliberately not detected; see dsp_core.h for the contract.
struct dg_engine {
    dg_engine(double sample_rate, int channels) noexcept
        : renderer(sample_rate, channels) {}

    dreamguard::SoundscapeRenderer renderer;
};

extern "C" {

dg_status dg_create(const dg_config* config, dg_engine** out_engine) {
    if (out_engine == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    *out_engine = nullptr;
    if (config == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (config->sample_rate < DG_SAMPLE_RATE_MIN ||
        config->sample_rate > DG_SAMPLE_RATE_MAX) {
        return DG_ERR_SAMPLE_RATE_OUT_OF_RANGE;
    }
    if (config->channel_count < DG_CHANNELS_MIN ||
        config->channel_count > DG_CHANNELS_MAX) {
        return DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE;
    }

    auto* engine = new (std::nothrow) dg_engine(
            static_cast<double>(config->sample_rate),
            static_cast<int>(config->channel_count));
    if (engine == nullptr) {
        return DG_ERR_OUT_OF_MEMORY;
    }

    *out_engine = engine;
    return DG_OK;
}

dg_status dg_destroy(dg_engine* engine) {
    if (engine == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    delete engine;
    return DG_OK;
}

dg_status dg_reset(dg_engine* engine) {
    if (engine == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    engine->renderer.reset();
    return DG_OK;
}

dg_status dg_render(dg_engine* engine,
                    float* interleaved,
                    uint32_t frame_count,
                    uint32_t* frames_written) {
    if (frames_written == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    *frames_written = 0;
    if (engine == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    if (interleaved == nullptr || frame_count == 0) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    const auto channels = static_cast<std::size_t>(engine->renderer.channels());
    const auto samples = static_cast<std::size_t>(frame_count) * channels;
    const int rendered = engine->renderer.render(
            std::span<float>(interleaved, samples));
    *frames_written = static_cast<uint32_t>(rendered);
    return DG_OK;
}

dg_status dg_set_gain(dg_engine* engine, float gain) {
    if (engine == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    engine->renderer.setGain(gain);
    return DG_OK;
}

dg_status dg_set_tone_cutoff_hz(dg_engine* engine, float cutoff_hz) {
    if (engine == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    engine->renderer.setToneCutoff(cutoff_hz);
    return DG_OK;
}

dg_status dg_set_noise_mix(dg_engine* engine, float mix) {
    if (engine == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    engine->renderer.setNoiseMix(mix);
    return DG_OK;
}

dg_status dg_set_drone_hz(dg_engine* engine, float hz) {
    if (engine == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    engine->renderer.setDroneHz(hz);
    return DG_OK;
}

const char* dg_version(void) {
    return dreamguard::kDspVersion;
}

int32_t dg_channel_count(const dg_engine* engine) {
    if (engine == nullptr) {
        return 0;
    }
    return static_cast<int32_t>(engine->renderer.channels());
}

}  // extern "C"
