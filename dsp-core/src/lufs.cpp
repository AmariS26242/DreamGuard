/* LUFS loudness analysis (DG-012) - provisional, see include/dreamguard/lufs.h.
 *
 * Algorithm (ITU-R BS.1770-4 / EBU R128):
 *   1. K-weight every channel with the two-stage filter (high shelf, then the
 *      RLB high-pass). Only the 48 kHz design is published; every other rate is
 *      the same analog prototype discretised with the bilinear transform.
 *   2. Accumulate z_window = sum over channels of mean square of the K-weighted
 *      signal over the window.
 *   3. loudness = -0.691 + 10*log10(z_window), in LUFS.
 *   4. Gating blocks are 400 ms of the last four 100 ms hops (75 % overlap).
 *      Integrated loudness applies the -70 LUFS absolute gate and then the
 *      -10 LU relative gate relative to the mean of the surviving blocks.
 *
 * Everything is deterministic and allocation-free after create: the block store
 * is a fixed double array, so chunking the input cannot change any result.
 */

#include "dreamguard/lufs.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int32_t kMaxChannels = 2;
constexpr uint32_t kMomentaryHops = 4;   /* 400 ms */
constexpr uint32_t kShortTermHops = 30;  /* 3 s */

constexpr double kNegativeInfinity = -std::numeric_limits<double>::infinity();

/* Same representability rule as pcm.cpp / meter.cpp: the view length must not
 * overflow size_t when scaled by the channel count. */
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

/* Stage 1 of K-weighting, from the BS.1770 analog prototype. At 48 kHz these
 * coefficients reproduce the published table; the deviation is checked by
 * dsp-core/tests/test_lufs.cpp. */
dg_biquad_coefficients shelf_coefficients(int32_t sample_rate) {
    const double f0 = 1681.974450955533;
    const double gain_db = 3.999843853973347;
    const double q = 0.7071752369554196;
    const double k = std::tan(kPi * f0 / static_cast<double>(sample_rate));
    const double vh = std::pow(10.0, gain_db / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;
    dg_biquad_coefficients c;
    c.b0 = (vh + vb * k / q + k * k) / a0;
    c.b1 = 2.0 * (k * k - vh) / a0;
    c.b2 = (vh - vb * k / q + k * k) / a0;
    c.a1 = 2.0 * (k * k - 1.0) / a0;
    c.a2 = (1.0 - k / q + k * k) / a0;
    return c;
}

/* Stage 2 of K-weighting (RLB high-pass). The numerator is exactly
 * (1 - z^-1)^2 for every rate; only a1/a2 are rate dependent. */
dg_biquad_coefficients high_pass_coefficients(int32_t sample_rate) {
    const double f0 = 38.13547087602444;
    const double q = 0.5003270373238773;
    const double k = std::tan(kPi * f0 / static_cast<double>(sample_rate));
    const double a0 = 1.0 + k / q + k * k;
    dg_biquad_coefficients c;
    c.b0 = 1.0;
    c.b1 = -2.0;
    c.b2 = 1.0;
    c.a1 = 2.0 * (k * k - 1.0) / a0;
    c.a2 = (1.0 - k / q + k * k) / a0;
    return c;
}

uint32_t hop_frames_for(int32_t sample_rate) {
    const double frames = static_cast<double>(sample_rate) / 10.0;
    const double rounded = std::floor(frames + 0.5);
    return rounded < 1.0 ? 1U : static_cast<uint32_t>(rounded);
}

double loudness_of(double mean_square) {
    if (!(mean_square > 0.0)) {
        return kNegativeInfinity;
    }
    return DG_LUFS_LOUDNESS_OFFSET + 10.0 * std::log10(mean_square);
}

struct BiquadState {
    double x1 = 0.0;
    double x2 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;
};

double biquad_step(const dg_biquad_coefficients& c, BiquadState* s, double x) {
    const double y = c.b0 * x + c.b1 * s->x1 + c.b2 * s->x2 - c.a1 * s->y1 -
                     c.a2 * s->y2;
    s->x2 = s->x1;
    s->x1 = x;
    s->y2 = s->y1;
    s->y1 = y;
    return y;
}

}  // namespace

struct dg_lufs {
    int32_t sample_rate = 0;
    int32_t channel_count = 0;
    uint32_t hop_frames = 0;
    uint32_t block_frames = 0;
    dg_biquad_coefficients shelf{};
    dg_biquad_coefficients high_pass{};

    BiquadState shelf_state[kMaxChannels];
    BiquadState high_pass_state[kMaxChannels];

    uint32_t hop_fill = 0;      /* frames accumulated toward the next hop */
    double hop_energy = 0.0;    /* sum over channels of K-weighted squares */
    double hop_history[kShortTermHops] = {};
    uint32_t hop_write = 0;     /* next slot in hop_history */
    uint64_t hop_count = 0;     /* completed hops since create/reset */

    double* blocks = nullptr;   /* one 400 ms mean square per completed hop */
    uint64_t block_count = 0;   /* blocks produced, including dropped ones */
    uint64_t blocks_stored = 0;
    uint64_t blocks_dropped = 0;

    uint64_t non_finite_samples = 0;
};

namespace {

void clear_analysis(dg_lufs* lufs) {
    for (int32_t ch = 0; ch < kMaxChannels; ++ch) {
        lufs->shelf_state[ch] = BiquadState{};
        lufs->high_pass_state[ch] = BiquadState{};
    }
    for (uint32_t i = 0; i < kShortTermHops; ++i) {
        lufs->hop_history[i] = 0.0;
    }
    lufs->hop_fill = 0;
    lufs->hop_energy = 0.0;
    lufs->hop_write = 0;
    lufs->hop_count = 0;
    for (uint64_t i = 0; i < lufs->blocks_stored; ++i) {
        lufs->blocks[i] = 0.0;
    }
    lufs->block_count = 0;
    lufs->blocks_stored = 0;
    lufs->blocks_dropped = 0;
    lufs->non_finite_samples = 0;
}

/* The k-th most recent completed hop, k = 0 being the newest. Requires
 * hop_count > k and k < kShortTermHops. */
double recent_hop(const dg_lufs* lufs, uint32_t k) {
    const uint32_t index =
        (lufs->hop_write + kShortTermHops - 1U - k) % kShortTermHops;
    return lufs->hop_history[index];
}

void finish_hop(dg_lufs* lufs) {
    lufs->hop_history[lufs->hop_write] = lufs->hop_energy;
    lufs->hop_write = (lufs->hop_write + 1U) % kShortTermHops;
    lufs->hop_energy = 0.0;
    lufs->hop_fill = 0;
    ++lufs->hop_count;

    if (lufs->hop_count < kMomentaryHops) {
        return;
    }
    /* 400 ms gating block: mean square over the last four hops. */
    double sum = 0.0;
    for (uint32_t k = 0; k < kMomentaryHops; ++k) {
        sum += recent_hop(lufs, k);
    }
    const double mean_square =
        sum / (static_cast<double>(kMomentaryHops) *
               static_cast<double>(lufs->hop_frames));
    ++lufs->block_count;
    /* Fixed store used as a ring: after one hour the oldest block is overwritten
     * so the integrated value keeps tracking the most recent hour instead of
     * freezing on the first one. The store itself never reallocates. */
    const uint64_t write_index = (lufs->block_count - 1U) % DG_LUFS_BLOCKS_CAPACITY;
    lufs->blocks[write_index] = mean_square;
    if (lufs->blocks_stored < DG_LUFS_BLOCKS_CAPACITY) {
        ++lufs->blocks_stored;
    } else {
        ++lufs->blocks_dropped;
    }
}

}  // namespace

extern "C" {

dg_status dg_lufs_create(const dg_lufs_config* config, dg_lufs** out_lufs) {
    if (out_lufs == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    *out_lufs = nullptr;
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

    double* blocks = new (std::nothrow) double[DG_LUFS_BLOCKS_CAPACITY];
    if (blocks == nullptr) {
        return DG_ERR_OUT_OF_MEMORY;
    }
    dg_lufs* lufs = new (std::nothrow) dg_lufs{};
    if (lufs == nullptr) {
        delete[] blocks;
        return DG_ERR_OUT_OF_MEMORY;
    }
    lufs->sample_rate = config->sample_rate;
    lufs->channel_count = config->channel_count;
    lufs->hop_frames = hop_frames_for(config->sample_rate);
    lufs->block_frames = lufs->hop_frames * kMomentaryHops;
    lufs->shelf = shelf_coefficients(config->sample_rate);
    lufs->high_pass = high_pass_coefficients(config->sample_rate);
    lufs->blocks = blocks;
    clear_analysis(lufs);
    *out_lufs = lufs;
    return DG_OK;
}

dg_status dg_lufs_destroy(dg_lufs* lufs) {
    if (lufs == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    delete[] lufs->blocks;
    delete lufs;
    return DG_OK;
}

dg_status dg_lufs_reset(dg_lufs* lufs) {
    if (lufs == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    clear_analysis(lufs);
    return DG_OK;
}

dg_status dg_lufs_process(dg_lufs* lufs, dg_pcm_view input) {
    if (lufs == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }
    if (!view_is_valid(input) || input.channel_count != lufs->channel_count) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (input.frame_count == 0) {
        return DG_OK;
    }

    const int32_t channels = lufs->channel_count;
    const uint32_t frames = input.frame_count;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const std::size_t base =
            static_cast<std::size_t>(frame) * static_cast<std::size_t>(channels);
        for (int32_t ch = 0; ch < channels; ++ch) {
            float sample = input.samples[base + static_cast<std::size_t>(ch)];
            if (!std::isfinite(sample)) {
                ++lufs->non_finite_samples;
                sample = 0.0F;
            }
            const double filtered =
                biquad_step(lufs->shelf, &lufs->shelf_state[ch], sample);
            const double weighted =
                biquad_step(lufs->high_pass, &lufs->high_pass_state[ch], filtered);
            lufs->hop_energy += weighted * weighted;
        }
        ++lufs->hop_fill;
        if (lufs->hop_fill == lufs->hop_frames) {
            finish_hop(lufs);
        }
    }
    return DG_OK;
}

dg_status dg_lufs_snapshot(const dg_lufs* lufs, dg_lufs_values* out_values) {
    if (out_values == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (lufs == nullptr) {
        return DG_ERR_NULL_HANDLE;
    }

    dg_lufs_values values{};
    values.momentary_lufs = kNegativeInfinity;
    values.short_term_lufs = kNegativeInfinity;
    values.integrated_lufs = kNegativeInfinity;
    values.blocks = lufs->block_count;
    values.blocks_dropped = lufs->blocks_dropped;
    values.non_finite_samples = lufs->non_finite_samples;
    values.hop_frames = lufs->hop_frames;
    values.block_frames = lufs->block_frames;
    values.pending_frames = lufs->hop_fill;

    if (lufs->hop_count >= kMomentaryHops) {
        double sum = 0.0;
        for (uint32_t k = 0; k < kMomentaryHops; ++k) {
            sum += recent_hop(lufs, k);
        }
        values.momentary_lufs =
            loudness_of(sum / (static_cast<double>(kMomentaryHops) *
                               static_cast<double>(lufs->hop_frames)));
    }
    if (lufs->hop_count >= kShortTermHops) {
        double sum = 0.0;
        for (uint32_t k = 0; k < kShortTermHops; ++k) {
            sum += recent_hop(lufs, k);
        }
        values.short_term_lufs =
            loudness_of(sum / (static_cast<double>(kShortTermHops) *
                               static_cast<double>(lufs->hop_frames)));
    }

    /* Absolute gate: keep blocks at or above -70 LUFS. */
    double absolute_sum = 0.0;
    uint64_t absolute_count = 0;
    for (uint64_t i = 0; i < lufs->blocks_stored; ++i) {
        if (loudness_of(lufs->blocks[i]) >= DG_LUFS_ABSOLUTE_GATE_LUFS) {
            absolute_sum += lufs->blocks[i];
            ++absolute_count;
        }
    }
    values.blocks_after_absolute_gate = absolute_count;

    if (absolute_count > 0) {
        /* Relative gate: discard blocks more than 10 LU below the mean of the
         * blocks that passed the absolute gate. */
        const double relative_threshold =
            (absolute_sum / static_cast<double>(absolute_count)) / 10.0;
        double gated_sum = 0.0;
        uint64_t gated_count = 0;
        for (uint64_t i = 0; i < lufs->blocks_stored; ++i) {
            const double block = lufs->blocks[i];
            /* BS.1770-4 keeps blocks whose loudness is >= the relative gate, i.e.
             * block energy >= mean energy / 10^(10/10). */
            if (block >= relative_threshold &&
                loudness_of(block) >= DG_LUFS_ABSOLUTE_GATE_LUFS) {
                gated_sum += block;
                ++gated_count;
            }
        }
        values.blocks_used = gated_count;
        if (gated_count > 0) {
            values.integrated_lufs =
                loudness_of(gated_sum / static_cast<double>(gated_count));
            values.integrated_ready = 1;
        }
    }

    *out_values = values;
    return DG_OK;
}

dg_status dg_lufs_k_weighting_for_rate(int32_t sample_rate,
                                       dg_lufs_k_weighting* out_weighting) {
    if (out_weighting == nullptr) {
        return DG_ERR_INVALID_ARGUMENT;
    }
    if (sample_rate < DG_SAMPLE_RATE_MIN || sample_rate > DG_SAMPLE_RATE_MAX) {
        return DG_ERR_SAMPLE_RATE_OUT_OF_RANGE;
    }
    out_weighting->shelf = shelf_coefficients(sample_rate);
    out_weighting->high_pass = high_pass_coefficients(sample_rate);
    return DG_OK;
}

}  // extern "C"
