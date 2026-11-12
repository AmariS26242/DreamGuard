#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <vector>

#include "dreamguard/compressor.h"
#include "dreamguard/limiter.h"
#include "dreamguard/true_peak.h"

namespace {

using Clock = std::chrono::steady_clock;

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

double percentile95(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        std::ceil(0.95 * static_cast<double>(values.size())) - 1.0);
    return values[std::min(index, values.size() - 1)];
}

void fill_signal(std::vector<float>& input, int32_t channels, int32_t sample_rate) {
    const double rate = static_cast<double>(sample_rate);
    for (std::size_t frame = 0; frame < input.size() / static_cast<std::size_t>(channels); ++frame) {
        const float sample = static_cast<float>(0.3 * std::sin(
            2.0 * 3.14159265358979323846 * 997.0 * static_cast<double>(frame) / rate));
        for (int32_t channel = 0; channel < channels; ++channel) {
            input[frame * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)] =
                channel == 0 ? sample : sample * 0.7F;
        }
    }
}

void print_row(const char* chain, int32_t rate, uint32_t block, int32_t channels,
               const std::vector<double>& samples, double xrt) {
    std::printf("%s,%d,%u,%d,%.3f,%.3f,%.6f\n",
                chain, rate, block, channels, median(samples), percentile95(samples), xrt);
}

void bench_one(int32_t rate, uint32_t block, int32_t channels, int repeats) {
    const std::size_t samples = static_cast<std::size_t>(block) * static_cast<std::size_t>(channels);
    std::vector<float> input(samples);
    std::vector<float> output(samples);
    fill_signal(input, channels, rate);

    dg_compressor_config compressor_config{};
    compressor_config.sample_rate = rate;
    compressor_config.channel_count = channels;
    compressor_config.threshold_db = -18.0;
    compressor_config.ratio = 2.0;
    compressor_config.knee_db = 6.0;
    compressor_config.attack_ms = 20.0;
    compressor_config.release_ms = 250.0;
    compressor_config.makeup_gain_db = -6.020599913;
    compressor_config.link_stereo = 1;

    dg_limiter_config limiter_config{};
    limiter_config.sample_rate = rate;
    limiter_config.channel_count = channels;
    limiter_config.ceiling_dbfs = -1.0;
    limiter_config.lookahead_ms = 2.0;
    limiter_config.release_ms = 250.0;
    limiter_config.link_stereo = 1;

    dg_true_peak_config true_peak_config{rate, channels};
    dg_compressor* compressor = nullptr;
    dg_limiter* limiter = nullptr;
    dg_true_peak* true_peak = nullptr;
    if (dg_compressor_create(&compressor_config, &compressor) != DG_OK ||
        dg_limiter_create(&limiter_config, &limiter) != DG_OK ||
        dg_true_peak_create(&true_peak_config, &true_peak) != DG_OK) {
        std::fprintf(stderr, "benchmark setup failed\n");
        std::exit(2);
    }

    std::vector<double> compressor_times;
    std::vector<double> limiter_times;
    std::vector<double> true_peak_times;
    compressor_times.reserve(repeats);
    limiter_times.reserve(repeats);
    true_peak_times.reserve(repeats);

    for (int repeat = 0; repeat < repeats; ++repeat) {
        auto start = Clock::now();
        dg_compressor_process(compressor, {input.data(), block, channels},
                              {output.data(), block, channels});
        const auto compressor_elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start).count();

        start = Clock::now();
        dg_limiter_process(limiter, {input.data(), block, channels},
                           {output.data(), block, channels});
        const auto limiter_elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start).count();

        start = Clock::now();
        dg_true_peak_process(true_peak, {input.data(), block, channels});
        const auto true_peak_elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start).count();

        compressor_times.push_back(compressor_elapsed);
        limiter_times.push_back(limiter_elapsed);
        true_peak_times.push_back(true_peak_elapsed);
    }

    const double audio_us = static_cast<double>(block) * 1'000'000.0 / static_cast<double>(rate);
    std::printf("chain,rate,block,channels,median_us,p95_us,xrt\n");
    print_row("compressor", rate, block, channels, compressor_times, audio_us / median(compressor_times));
    print_row("limiter", rate, block, channels, limiter_times, audio_us / median(limiter_times));
    print_row("true_peak", rate, block, channels, true_peak_times, audio_us / median(true_peak_times));

    dg_true_peak_destroy(true_peak);
    dg_limiter_destroy(limiter);
    dg_compressor_destroy(compressor);
}

}  // namespace

int main() {
    std::printf("dg_dsp_bench,steady_clock,one_block_per_sample,normal_priority\n");
    bench_one(48000, 256, 2, 25);
    bench_one(48000, 512, 2, 25);
    bench_one(96000, 512, 2, 25);
    return 0;
}
