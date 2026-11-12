/* Host tests for the DG-014 4x oversampled true-peak analyser.
 *
 * The analytic tests use a sine at fs/4 with a 45-degree phase. Its samples are
 * all +/-A/sqrt(2) while the reconstructed waveform peaks at A, so the sample
 * peak and the true peak differ by exactly 3.01 dB. That is the property this
 * suite uses to prove that the analyser really reconstructs between samples
 * instead of relabelling the sample peak.
 *
 * This suite does not test a limiter. It measures; it cannot make a stream
 * compliant with any standard.
 */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "dreamguard/true_peak.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void expect(bool condition, const char* expression, const char* file, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s:%d: %s\n", file, line, expression);
    }
}

void expect_status(dg_status actual, dg_status expected, const char* expression,
                   const char* file, int line) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("FAIL %s:%d: %s returned %d, expected %d\n", file, line, expression,
                    static_cast<int>(actual), static_cast<int>(expected));
    }
}

void expect_close(double actual, double expected, double tolerance, const char* expression,
                  const char* file, int line) {
    ++g_checks;
    if (!(std::fabs(actual - expected) <= tolerance)) {
        ++g_failures;
        std::printf("FAIL %s:%d: %s = %.12f, expected %.12f +/- %.12f\n", file, line,
                    expression, actual, expected, tolerance);
    }
}

#define CHECK(condition) expect((condition), #condition, __FILE__, __LINE__)
#define EXPECT_STATUS(expression, expected) \
    expect_status((expression), (expected), #expression, __FILE__, __LINE__)
#define CHECK_CLOSE(actual, expected, tolerance) \
    expect_close((actual), (expected), (tolerance), #actual, __FILE__, __LINE__)

constexpr double kPi = 3.14159265358979323846;

dg_true_peak_config base_config(int32_t rate = 48000, int32_t channels = 1) {
    dg_true_peak_config config{};
    config.sample_rate = rate;
    config.channel_count = channels;
    return config;
}

dg_pcm_view view_of(const std::vector<float>& samples, int32_t channels) {
    dg_pcm_view view{};
    view.samples = samples.data();
    view.frame_count = static_cast<uint32_t>(samples.size() /
                                             static_cast<std::size_t>(channels));
    view.channel_count = channels;
    return view;
}

/* A raised-cosine fade-in avoids the step from digital silence, which would
 * otherwise make the interpolation filter ring and turn an analytic sine test
 * into a test of the step response. */
std::vector<float> faded_sine(std::size_t frames, int32_t channels, double rate, double hz,
                              double amplitude, double phase, std::size_t fade_frames) {
    std::vector<float> samples(frames * static_cast<std::size_t>(channels), 0.0F);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        double envelope = 1.0;
        if (fade_frames > 0 && frame < fade_frames) {
            envelope = 0.5 - 0.5 * std::cos(kPi * static_cast<double>(frame) /
                                            static_cast<double>(fade_frames));
        }
        const double value =
            amplitude * envelope *
            std::sin(2.0 * kPi * hz * static_cast<double>(frame) / rate + phase);
        for (int32_t ch = 0; ch < channels; ++ch) {
            samples[frame * static_cast<std::size_t>(channels) +
                    static_cast<std::size_t>(ch)] = static_cast<float>(value);
        }
    }
    return samples;
}

std::vector<float> constant(std::size_t frames, int32_t channels, float value) {
    return std::vector<float>(frames * static_cast<std::size_t>(channels), value);
}

std::vector<float> faded_constant(std::size_t frames, int32_t channels, float value,
                                  std::size_t fade_frames) {
    std::vector<float> samples(frames * static_cast<std::size_t>(channels), 0.0F);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        double envelope = 1.0;
        if (fade_frames > 0 && frame < fade_frames) {
            envelope = 0.5 - 0.5 * std::cos(kPi * static_cast<double>(frame) /
                                            static_cast<double>(fade_frames));
        }
        for (int32_t ch = 0; ch < channels; ++ch) {
            samples[frame * static_cast<std::size_t>(channels) +
                    static_cast<std::size_t>(ch)] = value * static_cast<float>(envelope);
        }
    }
    return samples;
}

struct Analysis {
    bool created = false;
    bool processed = true;
    dg_true_peak_values values{};
};

/* Processes the stream in optional chunks. The analyser carries its filter
 * history across calls, so the totals must not depend on the chunk size. */
Analysis analyse(const dg_true_peak_config& config, const std::vector<float>& input,
                 uint32_t chunk_frames = 0) {
    Analysis result;
    dg_true_peak* true_peak = nullptr;
    if (dg_true_peak_create(&config, &true_peak) != DG_OK || true_peak == nullptr) {
        return result;
    }
    result.created = true;
    const int32_t channels = config.channel_count;
    const std::size_t total_frames =
        input.size() / static_cast<std::size_t>(channels);
    std::size_t offset = 0;
    while (offset < total_frames) {
        std::size_t take = total_frames - offset;
        if (chunk_frames != 0 && take > static_cast<std::size_t>(chunk_frames)) {
            take = static_cast<std::size_t>(chunk_frames);
        }
        dg_pcm_view view{};
        view.samples = input.data() + offset * static_cast<std::size_t>(channels);
        view.frame_count = static_cast<uint32_t>(take);
        view.channel_count = channels;
        if (dg_true_peak_process(true_peak, view) != DG_OK) {
            result.processed = false;
            break;
        }
        offset += take;
    }
    if (result.processed) {
        result.processed = dg_true_peak_snapshot(true_peak, &result.values) == DG_OK;
    }
    dg_true_peak_destroy(true_peak);
    return result;
}

void test_create_validation() {
    dg_true_peak* true_peak = nullptr;
    dg_true_peak_config config = base_config();

    EXPECT_STATUS(dg_true_peak_create(nullptr, &true_peak), DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_true_peak_create(&config, nullptr), DG_ERR_INVALID_ARGUMENT);
    CHECK(true_peak == nullptr);

    config = base_config();
    config.sample_rate = DG_SAMPLE_RATE_MIN - 1;
    EXPECT_STATUS(dg_true_peak_create(&config, &true_peak),
                  DG_ERR_SAMPLE_RATE_OUT_OF_RANGE);
    config = base_config();
    config.sample_rate = DG_SAMPLE_RATE_MAX + 1;
    EXPECT_STATUS(dg_true_peak_create(&config, &true_peak),
                  DG_ERR_SAMPLE_RATE_OUT_OF_RANGE);

    config = base_config();
    config.channel_count = 0;
    EXPECT_STATUS(dg_true_peak_create(&config, &true_peak),
                  DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE);
    config = base_config();
    config.channel_count = 3;
    EXPECT_STATUS(dg_true_peak_create(&config, &true_peak),
                  DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE);

    config = base_config();
    EXPECT_STATUS(dg_true_peak_create(&config, &true_peak), DG_OK);
    CHECK(true_peak != nullptr);
    if (true_peak != nullptr) {
        dg_true_peak_values values{};
        EXPECT_STATUS(dg_true_peak_snapshot(true_peak, &values), DG_OK);
        CHECK(values.frames == 0);
        CHECK(values.non_finite_samples == 0);
        CHECK(values.channel_count == 1);
        CHECK(values.oversampling_factor == DG_TRUE_PEAK_OVERSAMPLING);
        CHECK(values.oversampling_factor == 4);
        CHECK(values.filter_taps == 65);
        CHECK(values.filter_latency_frames == DG_TRUE_PEAK_FILTER_LATENCY);
        CHECK(values.total_sample_peak_linear == 0.0);
        CHECK(values.total_true_peak_linear == 0.0);
        CHECK_CLOSE(values.total_true_peak_dbfs, -240.0, 1e-9);
        EXPECT_STATUS(dg_true_peak_destroy(true_peak), DG_OK);
    }
    EXPECT_STATUS(dg_true_peak_destroy(nullptr), DG_ERR_NULL_HANDLE);
}

void test_null_and_descriptor_validation() {
    dg_true_peak_config config = base_config(48000, 2);
    dg_true_peak* true_peak = nullptr;
    EXPECT_STATUS(dg_true_peak_create(&config, &true_peak), DG_OK);
    if (true_peak == nullptr) {
        CHECK(false);
        return;
    }

    std::vector<float> input = faded_sine(64, 2, 48000.0, 1000.0, 0.5, 0.0, 0);
    const dg_pcm_view valid = view_of(input, 2);

    EXPECT_STATUS(dg_true_peak_process(nullptr, valid), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_true_peak_snapshot(true_peak, nullptr), DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_true_peak_snapshot(nullptr, nullptr), DG_ERR_INVALID_ARGUMENT);

    {
        dg_pcm_view empty{};
        empty.frame_count = 0;
        empty.channel_count = 0; /* a zero-frame call accepts any channel count */
        EXPECT_STATUS(dg_true_peak_process(true_peak, empty), DG_OK);
        empty.channel_count = 99;
        EXPECT_STATUS(dg_true_peak_process(true_peak, empty), DG_OK);
    }

    {
        dg_pcm_view bad = valid;
        bad.channel_count = 1;
        EXPECT_STATUS(dg_true_peak_process(true_peak, bad), DG_ERR_INVALID_ARGUMENT);
    }
    {
        dg_pcm_view bad = valid;
        bad.samples = nullptr;
        EXPECT_STATUS(dg_true_peak_process(true_peak, bad), DG_ERR_INVALID_ARGUMENT);
    }

    dg_true_peak_values values{};
    EXPECT_STATUS(dg_true_peak_snapshot(true_peak, &values), DG_OK);
    CHECK(values.frames == 0);
    CHECK(values.channel_count == 2);
    EXPECT_STATUS(dg_true_peak_destroy(true_peak), DG_OK);
}

void test_silence_and_dc() {
    {
        const Analysis result = analyse(base_config(), constant(4800, 1, 0.0F));
        CHECK(result.created && result.processed);
        CHECK(result.values.frames == 4800);
        CHECK(result.values.total_sample_peak_linear == 0.0);
        CHECK(result.values.total_true_peak_linear == 0.0);
        CHECK_CLOSE(result.values.total_sample_peak_dbfs, -240.0, 1e-9);
        CHECK_CLOSE(result.values.total_true_peak_dbfs, -240.0, 1e-9);
    }
    {
        /* A slow ramp to DC is reconstructed without a step transient. */
        const Analysis result =
            analyse(base_config(), faded_constant(4800, 1, 0.5F, 480));
        CHECK(result.created && result.processed);
        CHECK_CLOSE(result.values.total_sample_peak_linear, 0.5, 1e-7);
        CHECK_CLOSE(result.values.total_true_peak_linear, 0.5, 2e-3);
        CHECK(result.values.total_true_peak_linear >=
              result.values.total_sample_peak_linear - 1e-9);
    }
    {
        const Analysis result =
            analyse(base_config(), faded_constant(4800, 1, -0.5F, 480));
        CHECK(result.created && result.processed);
        CHECK_CLOSE(result.values.total_sample_peak_linear, 0.5, 1e-7);
        CHECK_CLOSE(result.values.total_true_peak_linear, 0.5, 2e-3);
    }
}

void test_fs4_45_degrees_analytic() {
    const double amplitude = 1.0;
    const std::vector<float> input =
        faded_sine(9600, 1, 48000.0, 12000.0, amplitude, kPi / 4.0, 480);
    const Analysis result = analyse(base_config(), input);
    CHECK(result.created && result.processed);
    if (!result.processed) {
        return;
    }

    /* Samples sit at +/-A/sqrt(2); the reconstructed waveform peaks at A. */
    const double expected_sample = amplitude / std::sqrt(2.0);
    CHECK_CLOSE(result.values.total_sample_peak_linear, expected_sample, 1e-5);
    CHECK_CLOSE(result.values.total_true_peak_linear, amplitude, 3e-3);
    CHECK(result.values.total_true_peak_linear > result.values.total_sample_peak_linear);
    const double gap_db =
        result.values.total_true_peak_dbfs - result.values.total_sample_peak_dbfs;
    CHECK_CLOSE(gap_db, 20.0 * std::log10(std::sqrt(2.0)), 0.02);
    std::printf("  fs/4 45deg: sample %.6f (%.3f dBFS), true %.6f (%.3f dBFS), gap %.3f dB\n",
                result.values.total_sample_peak_linear,
                result.values.total_sample_peak_dbfs,
                result.values.total_true_peak_linear,
                result.values.total_true_peak_dbfs, gap_db);
}

void test_nyquist_and_impulse() {
    {
        std::vector<float> input = faded_sine(4800, 1, 48000.0, 24000.0, 0.75, 0.0, 480);
        for (std::size_t i = 0; i < input.size(); ++i) {
            /* At exactly fs/2 the samples alternate; make them explicit rather
             * than relying on sin(pi * n) evaluating to exactly zero. */
            input[i] = (i % 2 == 0)
                           ? static_cast<float>(0.75 *
                                                (i < 480 ? 0.5 - 0.5 * std::cos(
                                                                 kPi * static_cast<double>(i) /
                                                                 480.0)
                                                         : 1.0))
                           : static_cast<float>(-0.75 *
                                                (i < 480 ? 0.5 - 0.5 * std::cos(
                                                                 kPi * static_cast<double>(i) /
                                                                 480.0)
                                                         : 1.0));
        }
        const Analysis result = analyse(base_config(), input);
        CHECK(result.created && result.processed);
        CHECK_CLOSE(result.values.total_sample_peak_linear, 0.75, 1e-6);
        /* The phase-0 branch is exact, so the sample peak is always included. */
        CHECK(result.values.total_true_peak_linear >= 0.75 - 1e-9);
        CHECK(result.values.total_true_peak_linear <= 0.75 * 1.05);
    }
    {
        std::vector<float> input(512, 0.0F);
        input[200] = 1.0F;
        const Analysis result = analyse(base_config(), input);
        CHECK(result.created && result.processed);
        CHECK_CLOSE(result.values.total_sample_peak_linear, 1.0, 1e-9);
        CHECK_CLOSE(result.values.total_true_peak_linear, 1.0, 1e-9);
    }
    {
        std::vector<float> input(512, 0.0F);
        input[200] = -1.0F;
        const Analysis result = analyse(base_config(), input);
        CHECK(result.created && result.processed);
        CHECK_CLOSE(result.values.total_sample_peak_linear, 1.0, 1e-9);
        CHECK_CLOSE(result.values.total_true_peak_linear, 1.0, 1e-9);
    }
}

void test_non_finite() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    {
        /* The trailing zeros let the 0.5 sample travel through the 65-tap
         * filter (8-frame centre delay) before the stream ends. */
        std::vector<float> input = {nan, inf, -inf, 0.5F};
        input.resize(37, 0.0F);
        const Analysis result = analyse(base_config(), input);
        CHECK(result.created && result.processed);
        CHECK(result.values.frames == 37);
        CHECK(result.values.non_finite_samples == 3);
        CHECK_CLOSE(result.values.total_sample_peak_linear, 0.5, 1e-9);
        CHECK_CLOSE(result.values.total_true_peak_linear, 0.5, 1e-9);
        CHECK(std::isfinite(result.values.total_true_peak_dbfs));
        CHECK(std::isfinite(result.values.sample_peak_dbfs));
        CHECK(std::isfinite(result.values.true_peak_dbfs));
    }
    {
        std::vector<float> input(64, nan);
        for (std::size_t i = 0; i < input.size(); i += 3) {
            input[i] = inf;
        }
        const Analysis result = analyse(base_config(), input);
        CHECK(result.created && result.processed);
        CHECK(result.values.non_finite_samples == 64);
        CHECK(result.values.total_sample_peak_linear == 0.0);
        CHECK(result.values.total_true_peak_linear == 0.0);
        CHECK_CLOSE(result.values.total_true_peak_dbfs, -240.0, 1e-9);
    }
}

void test_mono_and_stereo() {
    {
        std::vector<float> input(1000 * 2, 0.0F);
        for (std::size_t i = 0; i < 1000; ++i) {
            input[i * 2] = 0.5F;
            input[i * 2 + 1] = -1.25F;
        }
        const Analysis result = analyse(base_config(48000, 2), input);
        CHECK(result.created && result.processed);
        CHECK(result.values.channel_count == 2);
        CHECK_CLOSE(result.values.total_sample_peak_linear, 1.25, 1e-6);
        CHECK(result.values.total_true_peak_linear >= 1.25 - 1e-9);
        CHECK(result.values.frames == 1000);
    }
    {
        /* A quiet right channel must not borrow the left channel's peak. */
        std::vector<float> input = faded_constant(1000, 2, 0.5F, 100);
        for (std::size_t i = 0; i < 1000; ++i) {
            input[i * 2 + 1] = 0.0F;
        }
        const Analysis result = analyse(base_config(48000, 2), input);
        CHECK(result.created && result.processed);
        CHECK_CLOSE(result.values.total_sample_peak_linear, 0.5, 1e-7);
        CHECK_CLOSE(result.values.total_true_peak_linear, 0.5, 2e-3);
    }
}

void test_chunking_and_odd_blocks() {
    const std::vector<float> input =
        faded_sine(101, 1, 48000.0, 12000.0, 1.0, kPi / 4.0, 21);
    const Analysis whole = analyse(base_config(), input, 0);
    const Analysis one = analyse(base_config(), input, 1);
    const Analysis odd = analyse(base_config(), input, 3);
    const Analysis large = analyse(base_config(), input, 1000);
    CHECK(whole.created && whole.processed);
    CHECK(one.created && one.processed);
    CHECK(odd.created && odd.processed);
    CHECK(large.created && large.processed);
    CHECK(whole.values.frames == 101);
    CHECK(one.values.frames == 101);
    CHECK(odd.values.frames == 101);
    CHECK(large.values.frames == 101);
    /* Per-frame arithmetic is identical, so the totals are bit-identical. */
    CHECK(one.values.total_sample_peak_linear == whole.values.total_sample_peak_linear);
    CHECK(one.values.total_true_peak_linear == whole.values.total_true_peak_linear);
    CHECK(odd.values.total_sample_peak_linear == whole.values.total_sample_peak_linear);
    CHECK(odd.values.total_true_peak_linear == whole.values.total_true_peak_linear);
    CHECK(large.values.total_sample_peak_linear == whole.values.total_sample_peak_linear);
    CHECK(large.values.total_true_peak_linear == whole.values.total_true_peak_linear);
    CHECK(one.values.non_finite_samples == whole.values.non_finite_samples);
}

void test_reset() {
    dg_true_peak_config config = base_config();
    dg_true_peak* true_peak = nullptr;
    EXPECT_STATUS(dg_true_peak_create(&config, &true_peak), DG_OK);
    if (true_peak == nullptr) {
        CHECK(false);
        return;
    }
    EXPECT_STATUS(dg_true_peak_reset(nullptr), DG_ERR_NULL_HANDLE);

    std::vector<float> loud = faded_constant(64, 1, 1.0F, 16);
    dg_true_peak_values values{};
    EXPECT_STATUS(dg_true_peak_process(true_peak, view_of(loud, 1)), DG_OK);
    EXPECT_STATUS(dg_true_peak_snapshot(true_peak, &values), DG_OK);
    CHECK(values.frames == 64);
    CHECK_CLOSE(values.total_sample_peak_linear, 1.0, 1e-6);
    CHECK_CLOSE(values.total_true_peak_linear, 1.0, 2e-3);

    EXPECT_STATUS(dg_true_peak_reset(true_peak), DG_OK);
    EXPECT_STATUS(dg_true_peak_snapshot(true_peak, &values), DG_OK);
    CHECK(values.frames == 0);
    CHECK(values.non_finite_samples == 0);
    CHECK(values.sample_peak_linear == 0.0);
    CHECK(values.true_peak_linear == 0.0);
    CHECK(values.total_sample_peak_linear == 0.0);
    CHECK(values.total_true_peak_linear == 0.0);
    CHECK_CLOSE(values.total_true_peak_dbfs, -240.0, 1e-9);

    const Analysis after = analyse(config, faded_constant(1000, 1, 0.25F, 100));
    CHECK(after.created && after.processed);
    CHECK(after.values.frames == 1000);
    CHECK_CLOSE(after.values.total_sample_peak_linear, 0.25, 1e-7);
    CHECK_CLOSE(after.values.total_true_peak_linear, 0.25, 2e-3);
    EXPECT_STATUS(dg_true_peak_destroy(true_peak), DG_OK);
}

void test_rate_and_channel_smoke() {
    const int32_t rates[] = {8000, 44100, 48000, 96000, 192000};
    for (int32_t rate : rates) {
        for (int32_t channels = 1; channels <= 2; ++channels) {
            const std::size_t frames = static_cast<std::size_t>(rate) / 10U;
            const std::size_t fade = static_cast<std::size_t>(rate) / 100U;
            const std::vector<float> input =
                faded_sine(frames, channels, rate, rate / 4.0, 0.5, kPi / 4.0, fade);
            const Analysis result = analyse(base_config(rate, channels), input, 37);
            CHECK(result.created && result.processed);
            CHECK(result.values.frames == frames);
            CHECK(result.values.channel_count == channels);
            CHECK(result.values.oversampling_factor == 4);
            CHECK(result.values.filter_taps == 65);
            CHECK(std::isfinite(result.values.total_sample_peak_dbfs));
            CHECK(std::isfinite(result.values.total_true_peak_dbfs));
            CHECK(result.values.total_true_peak_linear >=
                  result.values.total_sample_peak_linear - 1e-9);
            CHECK(result.values.total_true_peak_linear <= 0.5 * 1.02);
        }
    }
}

void test_dbfs_helper() {
    CHECK_CLOSE(dg_true_peak_linear_to_dbfs(1.0), 0.0, 1e-12);
    CHECK_CLOSE(dg_true_peak_linear_to_dbfs(0.5), -6.020599913279624, 1e-12);
    CHECK_CLOSE(dg_true_peak_linear_to_dbfs(0.0), -240.0, 1e-9);
    CHECK_CLOSE(dg_true_peak_linear_to_dbfs(-1.0), -240.0, 1e-9);
    CHECK(std::isinf(dg_true_peak_linear_to_dbfs(
        std::numeric_limits<double>::infinity())));
    CHECK(std::isnan(dg_true_peak_linear_to_dbfs(
        std::numeric_limits<double>::quiet_NaN())));
}

void run(const char* name, void (*function)()) {
    const int before = g_failures;
    function();
    std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name);
}

}  // namespace

int main() {
    std::printf("dg_true_peak tests (DG-014, 4x oversampled measurement only)\n");

    run("create validation", test_create_validation);
    run("null handles and descriptor validation", test_null_and_descriptor_validation);
    run("silence and DC", test_silence_and_dc);
    run("fs/4 45-degree analytic gap", test_fs4_45_degrees_analytic);
    run("Nyquist and impulse", test_nyquist_and_impulse);
    run("non-finite input", test_non_finite);
    run("mono and stereo", test_mono_and_stereo);
    run("one-frame and odd chunks", test_chunking_and_odd_blocks);
    run("reset", test_reset);
    run("rate and channel smoke", test_rate_and_channel_smoke);
    run("dBFS helper", test_dbfs_helper);

    std::printf("  %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
