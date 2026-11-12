/* Host tests for the DG-014 sample-peak lookahead limiter.
 *
 * The limiter tests deliberately check a SAMPLE bound. One additional test at
 * the end feeds the limiter output to the DG-014 4x true-peak analyser to show
 * that the current limiter can still overshoot the inter-sample peak. That test
 * measures the gap only: it does not change the limiter algorithm, and a passing
 * run must never be reported as true-peak acceptance.
 */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "dreamguard/limiter.h"
#include "dreamguard/true_peak.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

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
        std::printf("FAIL %s:%d: %s = %.9f, expected %.9f +/- %.9f\n", file, line,
                    expression, actual, expected, tolerance);
    }
}

#define CHECK(condition) expect((condition), #condition, __FILE__, __LINE__)
#define EXPECT_STATUS(expression, expected) \
    expect_status((expression), (expected), #expression, __FILE__, __LINE__)
#define CHECK_CLOSE(actual, expected, tolerance) \
    expect_close((actual), (expected), (tolerance), #actual, __FILE__, __LINE__)

double dbfs_to_linear(double dbfs) {
    return std::pow(10.0, dbfs / 20.0);
}

dg_limiter_config base_config(int32_t rate = 48000, int32_t channels = 1) {
    dg_limiter_config config{};
    config.sample_rate = rate;
    config.channel_count = channels;
    config.ceiling_dbfs = -1.0;
    config.lookahead_ms = 5.0;
    config.release_ms = 50.0;
    config.link_stereo = 1;
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

dg_pcm_buffer buffer_of(std::vector<float>& samples, int32_t channels) {
    dg_pcm_buffer buffer{};
    buffer.samples = samples.data();
    buffer.frame_count = static_cast<uint32_t>(samples.size() /
                                               static_cast<std::size_t>(channels));
    buffer.channel_count = channels;
    return buffer;
}

struct RunResult {
    bool created = false;
    bool processed = false;
    bool flushed = false;
    uint32_t latency = 0;
    uint32_t flushed_frames = 0;
    std::vector<float> output;
    dg_limiter_values values{};
};

/* Processes the whole stream in optional chunks, snapshots after the last
 * process call, then flushes the lookahead tail and appends it. */
RunResult run_stream(const dg_limiter_config& config, const std::vector<float>& input,
                     uint32_t chunk_frames = 0) {
    RunResult result;
    const int32_t channels = config.channel_count;
    const std::size_t total_frames =
        input.size() / static_cast<std::size_t>(channels);

    dg_limiter* limiter = nullptr;
    if (dg_limiter_create(&config, &limiter) != DG_OK || limiter == nullptr) {
        return result;
    }
    result.created = true;
    result.latency = dg_limiter_latency_frames(limiter);

    result.output.assign(total_frames * static_cast<std::size_t>(channels), 0.0F);
    bool ok = true;
    if (chunk_frames == 0) {
        dg_pcm_view in = view_of(input, channels);
        dg_pcm_buffer out = buffer_of(result.output, channels);
        ok = dg_limiter_process(limiter, in, out) == DG_OK;
    } else {
        std::size_t offset = 0;
        while (offset < total_frames && ok) {
            const std::size_t remaining = total_frames - offset;
            const std::size_t take =
                remaining < static_cast<std::size_t>(chunk_frames)
                    ? remaining
                    : static_cast<std::size_t>(chunk_frames);
            dg_pcm_view in{};
            in.samples = input.data() + offset * static_cast<std::size_t>(channels);
            in.frame_count = static_cast<uint32_t>(take);
            in.channel_count = channels;
            dg_pcm_buffer out{};
            out.samples = result.output.data() + offset * static_cast<std::size_t>(channels);
            out.frame_count = static_cast<uint32_t>(take);
            out.channel_count = channels;
            if (dg_limiter_process(limiter, in, out) != DG_OK) {
                ok = false;
            }
            offset += take;
        }
    }
    result.processed = ok;
    if (ok) {
        ok = dg_limiter_snapshot(limiter, &result.values) == DG_OK;
    }

    if (ok && result.latency > 0) {
        std::vector<float> tail(static_cast<std::size_t>(result.latency) *
                                    static_cast<std::size_t>(channels),
                                0.0F);
        dg_pcm_buffer tail_buffer = buffer_of(tail, channels);
        const dg_status status =
            dg_limiter_flush(limiter, tail_buffer, &result.flushed_frames);
        result.flushed = status == DG_OK && result.flushed_frames == result.latency;
        if (result.flushed) {
            result.output.insert(result.output.end(), tail.begin(), tail.end());
        }
    } else if (ok) {
        result.flushed = true;
        result.flushed_frames = 0;
    }

    dg_limiter_destroy(limiter);
    return result;
}

double max_abs(const std::vector<float>& samples) {
    double peak = 0.0;
    for (float sample : samples) {
        if (std::isfinite(sample) && std::fabs(static_cast<double>(sample)) > peak) {
            peak = std::fabs(static_cast<double>(sample));
        }
    }
    return peak;
}

bool all_finite(const std::vector<float>& samples) {
    for (float sample : samples) {
        if (!std::isfinite(sample)) {
            return false;
        }
    }
    return true;
}

std::vector<float> sine(std::size_t frames, int32_t channels, double rate, double hz,
                        double amplitude) {
    std::vector<float> samples(frames * static_cast<std::size_t>(channels), 0.0F);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double value =
            amplitude * std::sin(2.0 * 3.14159265358979323846 * hz *
                                 static_cast<double>(frame) / rate);
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

void test_create_validation() {
    dg_limiter* limiter = nullptr;
    dg_limiter_config config = base_config();

    EXPECT_STATUS(dg_limiter_create(nullptr, &limiter), DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_limiter_create(&config, nullptr), DG_ERR_INVALID_ARGUMENT);
    CHECK(limiter == nullptr);

    config = base_config();
    config.sample_rate = DG_SAMPLE_RATE_MIN - 1;
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_ERR_SAMPLE_RATE_OUT_OF_RANGE);

    config = base_config();
    config.sample_rate = DG_SAMPLE_RATE_MAX + 1;
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_ERR_SAMPLE_RATE_OUT_OF_RANGE);

    config = base_config();
    config.channel_count = 0;
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE);

    config = base_config();
    config.channel_count = 3;
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE);

    config = base_config();
    config.ceiling_dbfs = DG_LIMITER_CEILING_DBFS_MIN - 0.1;
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_ERR_INVALID_ARGUMENT);

    config = base_config();
    config.ceiling_dbfs = DG_LIMITER_CEILING_DBFS_MAX + 0.1;
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_ERR_INVALID_ARGUMENT);

    config = base_config();
    config.ceiling_dbfs = std::numeric_limits<double>::quiet_NaN();
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_ERR_INVALID_ARGUMENT);

    config = base_config();
    config.lookahead_ms = DG_LIMITER_LOOKAHEAD_MS_MIN - 0.1;
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_ERR_INVALID_ARGUMENT);

    config = base_config();
    config.lookahead_ms = DG_LIMITER_LOOKAHEAD_MS_MAX + 0.1;
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_ERR_INVALID_ARGUMENT);

    config = base_config();
    config.release_ms = DG_LIMITER_RELEASE_MS_MIN - 0.1;
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_ERR_INVALID_ARGUMENT);

    config = base_config();
    config.release_ms = DG_LIMITER_RELEASE_MS_MAX + 0.1;
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_ERR_INVALID_ARGUMENT);

    config = base_config();
    config.link_stereo = 2;
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_ERR_INVALID_ARGUMENT);

    config = base_config();
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_OK);
    CHECK(limiter != nullptr);
    if (limiter != nullptr) {
        EXPECT_STATUS(dg_limiter_destroy(limiter), DG_OK);
    }
    EXPECT_STATUS(dg_limiter_destroy(nullptr), DG_ERR_NULL_HANDLE);
}

void test_null_and_descriptor_validation() {
    dg_limiter_config config = base_config(48000, 2);
    dg_limiter* limiter = nullptr;
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_OK);
    if (limiter == nullptr) {
        CHECK(false);
        return;
    }

    std::vector<float> input = sine(64, 2, 48000.0, 1000.0, 0.5);
    std::vector<float> output(input.size(), 0.0F);
    dg_pcm_view in = view_of(input, 2);
    dg_pcm_buffer out = buffer_of(output, 2);

    EXPECT_STATUS(dg_limiter_process(nullptr, in, out), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_limiter_snapshot(limiter, nullptr), DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_limiter_snapshot(nullptr, nullptr), DG_ERR_INVALID_ARGUMENT);

    {
        dg_pcm_view empty{};
        empty.frame_count = 0;
        empty.channel_count = 0;
        dg_pcm_buffer target{};
        target.frame_count = 0;
        target.channel_count = 0;
        EXPECT_STATUS(dg_limiter_process(limiter, empty, target), DG_OK);
    }

    {
        dg_pcm_view bad = in;
        bad.channel_count = 1;
        EXPECT_STATUS(dg_limiter_process(limiter, bad, out), DG_ERR_INVALID_ARGUMENT);
    }
    {
        dg_pcm_view bad = in;
        bad.frame_count = 63;
        EXPECT_STATUS(dg_limiter_process(limiter, bad, out), DG_ERR_INVALID_ARGUMENT);
    }
    {
        dg_pcm_view bad = in;
        bad.samples = nullptr;
        EXPECT_STATUS(dg_limiter_process(limiter, bad, out), DG_ERR_INVALID_ARGUMENT);
    }
    {
        /* One float of overlap inside the same output buffer, not the exact
         * in-place pointer. */
        dg_pcm_view overlapping{};
        overlapping.samples = output.data() + 1;
        overlapping.frame_count = 32;
        overlapping.channel_count = 2;
        dg_pcm_buffer target{};
        target.samples = output.data();
        target.frame_count = 32;
        target.channel_count = 2;
        EXPECT_STATUS(dg_limiter_process(limiter, overlapping, target),
                      DG_ERR_INVALID_ARGUMENT);
    }

    {
        uint32_t flushed = 123;
        EXPECT_STATUS(dg_limiter_flush(limiter, out, nullptr), DG_ERR_INVALID_ARGUMENT);
        dg_pcm_buffer bad = out;
        bad.channel_count = 1;
        EXPECT_STATUS(dg_limiter_flush(limiter, bad, &flushed), DG_ERR_INVALID_ARGUMENT);
        CHECK(flushed == 0);
        bad = out;
        bad.frame_count = 1; /* latency is 240 frames (5 ms at 48 kHz) */
        EXPECT_STATUS(dg_limiter_flush(limiter, bad, &flushed), DG_ERR_INVALID_ARGUMENT);
        bad = out;
        bad.samples = nullptr;
        EXPECT_STATUS(dg_limiter_flush(limiter, bad, &flushed), DG_ERR_INVALID_ARGUMENT);
    }

    CHECK(dg_limiter_latency_frames(nullptr) == 0);
    {
        uint32_t flushed = 0;
        dg_pcm_buffer empty{};
        empty.frame_count = 0;
        empty.channel_count = 2;
        EXPECT_STATUS(dg_limiter_flush(limiter, empty, &flushed), DG_ERR_INVALID_ARGUMENT);
    }

    EXPECT_STATUS(dg_limiter_destroy(limiter), DG_OK);
}

void test_latency_and_passthrough() {
    dg_limiter_config config = base_config(48000, 1);
    config.ceiling_dbfs = -1.0;
    config.lookahead_ms = 5.0;
    const std::vector<float> input = sine(4800, 1, 48000.0, 997.0, 0.5);
    const RunResult result = run_stream(config, input);

    CHECK(result.created);
    CHECK(result.processed);
    CHECK(result.flushed);
    CHECK(result.latency == 240);
    CHECK(result.output.size() == input.size() + result.latency);
    for (uint32_t i = 0; i < result.latency; ++i) {
        CHECK(result.output[i] == 0.0F);
    }
    bool delayed_exactly = true;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (std::fabs(static_cast<double>(result.output[i + result.latency]) -
                      static_cast<double>(input[i])) > 1e-7) {
            delayed_exactly = false;
        }
    }
    CHECK(delayed_exactly);
    CHECK_CLOSE(result.values.current_gain_db, 0.0, 1e-9);
    CHECK(result.values.over_ceiling_samples == 0);
}

void test_impulse_latency() {
    dg_limiter_config config = base_config(48000, 1);
    config.ceiling_dbfs = -6.0;
    config.lookahead_ms = 5.0;
    std::vector<float> input(1000, 0.0F);
    input[0] = 1.0F;

    const RunResult result = run_stream(config, input);
    CHECK(result.created && result.processed && result.flushed);
    CHECK(result.latency == 240);
    CHECK(result.output.size() == 1240);
    for (uint32_t i = 0; i < result.latency; ++i) {
        CHECK(result.output[i] == 0.0F);
    }
    CHECK_CLOSE(result.output[240], dbfs_to_linear(-6.0), 1e-6);
    for (std::size_t i = 241; i < result.output.size(); ++i) {
        CHECK(result.output[i] == 0.0F);
    }
    CHECK_CLOSE(result.values.input_peak_db, 0.0, 1e-9);
    CHECK_CLOSE(result.values.output_peak_db, -6.0, 1e-6);
    CHECK(result.values.limited_samples == 1);
    CHECK(result.values.over_ceiling_samples == 0);
}

void test_sample_peak_bound() {
    const double ceilings[] = {-0.1, -1.0, -6.0, -20.0};
    const int32_t rates[] = {8000, 48000, 192000};
    const double looks[] = {0.0, 1.0, 10.0};
    for (double ceiling : ceilings) {
        for (int32_t rate : rates) {
            for (double look : looks) {
                for (int32_t channels = 1; channels <= 2; ++channels) {
                    dg_limiter_config config = base_config(rate, channels);
                    config.ceiling_dbfs = ceiling;
                    config.lookahead_ms = look;
                    config.release_ms = 50.0;
                    const std::size_t frames = static_cast<std::size_t>(rate) / 5U;
                    std::vector<float> input =
                        sine(frames, channels, rate, 997.0, 1.0);
                    /* Impulses on top of the sine, including the last frame. */
                    input[0] = 1.0F;
                    input[frames / 2] = -1.0F;
                    input[input.size() - static_cast<std::size_t>(channels)] = 1.0F;

                    const RunResult result = run_stream(config, input);
                    CHECK(result.created && result.processed && result.flushed);
                    if (!result.processed) {
                        continue;
                    }
                    CHECK(all_finite(result.output));
                    const double limit = dbfs_to_linear(ceiling);
                    CHECK(max_abs(result.output) <= limit + 1e-6);
                    CHECK(result.values.over_ceiling_samples == 0);
                    CHECK(result.values.limited_samples > 0);
                    CHECK(std::isfinite(result.values.current_gain_db));
                }
            }
        }
    }
}

void test_stereo_link() {
    dg_limiter_config config = base_config(48000, 2);
    config.ceiling_dbfs = -6.0;
    config.lookahead_ms = 0.0;
    const std::size_t frames = 480;
    std::vector<float> input(frames * 2, 0.0F);
    for (std::size_t i = 0; i < frames; ++i) {
        input[i * 2] = 0.9F;
        input[i * 2 + 1] = 0.1F;
    }

    const RunResult linked = run_stream(config, input);
    CHECK(linked.created && linked.processed && linked.flushed);
    CHECK(linked.latency == 0);
    const double ceiling = dbfs_to_linear(-6.0);
    CHECK_CLOSE(linked.output[400], ceiling, 1e-6);
    CHECK_CLOSE(linked.output[401], ceiling * (0.1 / 0.9), 1e-6);
    const double left_ratio = linked.output[400] / 0.9;
    const double right_ratio = linked.output[401] / 0.1;
    CHECK_CLOSE(left_ratio, right_ratio, 1e-6);

    config.link_stereo = 0;
    const RunResult unlinked = run_stream(config, input);
    CHECK(unlinked.created && unlinked.processed && unlinked.flushed);
    CHECK_CLOSE(unlinked.output[400], ceiling, 1e-6);
    CHECK_CLOSE(unlinked.output[401], 0.1, 1e-6);
}

void test_chunking_invariance() {
    dg_limiter_config config = base_config(48000, 2);
    config.lookahead_ms = 5.0;
    config.ceiling_dbfs = -3.0;

    const std::size_t frames = 4096;
    std::vector<float> input = sine(frames, 2, 48000.0, 441.0, 1.0);
    for (std::size_t i = 0; i < frames; i += 257) {
        input[i * 2] = 1.0F;
        input[i * 2 + 1] = -1.0F;
    }

    const RunResult one = run_stream(config, input, 0);
    const RunResult chunked = run_stream(config, input, 37);
    CHECK(one.created && one.processed && one.flushed);
    CHECK(chunked.created && chunked.processed && chunked.flushed);
    CHECK(one.output.size() == chunked.output.size());
    bool identical = one.output.size() == chunked.output.size();
    for (std::size_t i = 0; identical && i < one.output.size(); ++i) {
        if (one.output[i] != chunked.output[i]) {
            identical = false;
        }
    }
    CHECK(identical);
    CHECK(one.values.frames == chunked.values.frames);
    CHECK(one.values.limited_samples == chunked.values.limited_samples);
}

void test_full_window_increasing_required_gain() {
    /* A slowly decaying ramp makes every required gain larger than the last,
     * so the sliding-minimum deque fills to window_size and stays full. This is
     * the case where an insert-before-expire implementation would overwrite the
     * front; the sample bound must still hold. */
    dg_limiter_config config = base_config(48000, 1);
    config.ceiling_dbfs = -20.0;
    config.lookahead_ms = 5.0;
    const std::size_t frames = 2000;
    std::vector<float> input(frames, 1.0F);
    for (std::size_t i = 1; i < frames; ++i) {
        input[i] = 1.0F - 0.5F * static_cast<float>(i) /
                               static_cast<float>(frames - 1);
    }

    const RunResult result = run_stream(config, input);
    CHECK(result.created && result.processed && result.flushed);
    CHECK(result.latency == 240);
    CHECK(max_abs(result.output) <= dbfs_to_linear(-20.0) + 1e-6);
    CHECK(result.values.over_ceiling_samples == 0);
}

void test_flush_tail() {
    dg_limiter_config config = base_config(48000, 1);
    config.lookahead_ms = 5.0;
    /* 0 dBFS keeps the ramp below the ceiling, so this test isolates the
     * delay/flush path rather than the gain path. */
    config.ceiling_dbfs = 0.0;
    std::vector<float> input(1000, 0.0F);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<float>(i) / 1000.0F;
    }

    dg_limiter* limiter = nullptr;
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_OK);
    if (limiter == nullptr) {
        CHECK(false);
        return;
    }
    const uint32_t latency = dg_limiter_latency_frames(limiter);
    CHECK(latency == 240);
    std::vector<float> output(input.size(), 0.0F);
    EXPECT_STATUS(dg_limiter_process(limiter, view_of(input, 1), buffer_of(output, 1)),
                  DG_OK);

    std::vector<float> tail(latency, 0.0F);
    uint32_t flushed = 0;
    EXPECT_STATUS(dg_limiter_flush(limiter, buffer_of(tail, 1), &flushed), DG_OK);
    CHECK(flushed == latency);
    CHECK_CLOSE(tail[latency - 1], input.back(), 1e-7);
    CHECK_CLOSE(tail[0], input[input.size() - latency], 1e-7);

    dg_limiter_values values{};
    EXPECT_STATUS(dg_limiter_snapshot(limiter, &values), DG_OK);
    CHECK(values.frames == input.size());
    CHECK_CLOSE(values.current_gain_db, 0.0, 1e-9);

    /* After flush the handle is ready for a new stream: the delay starts at
     * zero again, so the first output frames are silent until the delay has
     * been filled. */
    std::vector<float> second(512, 0.5F);
    std::vector<float> second_out(second.size(), 1.0F);
    EXPECT_STATUS(dg_limiter_process(limiter, view_of(second, 1),
                                     buffer_of(second_out, 1)),
                  DG_OK);
    CHECK(second_out[0] == 0.0F);
    CHECK(second_out[latency] == 0.5F);

    EXPECT_STATUS(dg_limiter_destroy(limiter), DG_OK);

    /* Zero-latency flush is a valid no-op. */
    config.lookahead_ms = 0.0;
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_OK);
    uint32_t zero_flushed = 123;
    dg_pcm_buffer empty{};
    empty.samples = nullptr;
    empty.frame_count = 0;
    empty.channel_count = 1;
    EXPECT_STATUS(dg_limiter_flush(limiter, empty, &zero_flushed), DG_OK);
    CHECK(zero_flushed == 0);
    EXPECT_STATUS(dg_limiter_destroy(limiter), DG_OK);
}

void test_reset() {
    dg_limiter_config config = base_config(48000, 1);
    config.lookahead_ms = 0.0;
    config.ceiling_dbfs = -6.0;
    dg_limiter* limiter = nullptr;
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_OK);
    if (limiter == nullptr) {
        CHECK(false);
        return;
    }

    std::vector<float> loud(1, 1.0F);
    std::vector<float> out(1, 0.0F);
    EXPECT_STATUS(dg_limiter_process(limiter, view_of(loud, 1), buffer_of(out, 1)), DG_OK);
    dg_limiter_values values{};
    EXPECT_STATUS(dg_limiter_snapshot(limiter, &values), DG_OK);
    CHECK(values.frames == 1);
    CHECK(values.limited_samples == 1);
    CHECK(values.current_gain_db < 0.0);

    EXPECT_STATUS(dg_limiter_reset(limiter), DG_OK);
    EXPECT_STATUS(dg_limiter_snapshot(limiter, &values), DG_OK);
    CHECK(values.frames == 0);
    CHECK(values.limited_samples == 0);
    CHECK(values.over_ceiling_samples == 0);
    CHECK_CLOSE(values.current_gain_db, 0.0, 1e-9);

    std::vector<float> quiet(1, 0.1F);
    EXPECT_STATUS(dg_limiter_process(limiter, view_of(quiet, 1), buffer_of(out, 1)), DG_OK);
    CHECK_CLOSE(out[0], 0.1, 1e-7);

    EXPECT_STATUS(dg_limiter_destroy(limiter), DG_OK);
}

void test_in_place() {
    dg_limiter_config config = base_config(48000, 1);
    config.lookahead_ms = 2.0;
    config.ceiling_dbfs = -2.0;

    std::vector<float> input = sine(2048, 1, 48000.0, 997.0, 1.0);
    input[0] = 1.0F;
    const RunResult separate = run_stream(config, input);
    CHECK(separate.created && separate.processed && separate.flushed);

    dg_limiter* limiter = nullptr;
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_OK);
    if (limiter == nullptr) {
        CHECK(false);
        return;
    }
    std::vector<float> in_place = input;
    EXPECT_STATUS(dg_limiter_process(limiter, view_of(in_place, 1),
                                     buffer_of(in_place, 1)),
                  DG_OK);
    bool identical = true;
    for (std::size_t i = 0; i < in_place.size(); ++i) {
        if (in_place[i] != separate.output[i]) {
            identical = false;
        }
    }
    CHECK(identical);
    EXPECT_STATUS(dg_limiter_destroy(limiter), DG_OK);
}

void test_non_finite_input() {
    dg_limiter_config config = base_config(48000, 1);
    config.lookahead_ms = 0.0;
    dg_limiter* limiter = nullptr;
    EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_OK);
    if (limiter == nullptr) {
        CHECK(false);
        return;
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> input = {nan, inf, -inf, 0.5F};
    std::vector<float> output(input.size(), 0.0F);
    EXPECT_STATUS(dg_limiter_process(limiter, view_of(input, 1), buffer_of(output, 1)),
                  DG_OK);
    CHECK(all_finite(output));
    CHECK(output[0] == 0.0F);
    CHECK(output[1] == 0.0F);
    CHECK(output[2] == 0.0F);
    CHECK_CLOSE(output[3], 0.5, 1e-7);

    dg_limiter_values values{};
    EXPECT_STATUS(dg_limiter_snapshot(limiter, &values), DG_OK);
    CHECK(values.non_finite_samples == 3);
    CHECK(values.over_ceiling_samples == 0);
    EXPECT_STATUS(dg_limiter_destroy(limiter), DG_OK);
}

void test_extremes() {
    {
        dg_limiter_config config = base_config(48000, 1);
        config.ceiling_dbfs = -60.0;
        config.lookahead_ms = 1.0;
        const RunResult result = run_stream(config, constant(4800, 1, 1.0F));
        CHECK(result.created && result.processed && result.flushed);
        CHECK(max_abs(result.output) <= dbfs_to_linear(-60.0) + 1e-6);
        CHECK(result.values.over_ceiling_samples == 0);
    }
    {
        /* Both ends of the release range must be accepted and usable. */
        dg_limiter_config config = base_config(48000, 1);
        config.ceiling_dbfs = -1.0;
        config.lookahead_ms = 1.0;
        config.release_ms = DG_LIMITER_RELEASE_MS_MIN;
        const RunResult fast = run_stream(config, constant(4800, 1, 1.0F));
        CHECK(fast.created && fast.processed && fast.flushed);
        CHECK(max_abs(fast.output) <= dbfs_to_linear(-1.0) + 1e-6);
        CHECK(fast.values.over_ceiling_samples == 0);

        config.release_ms = DG_LIMITER_RELEASE_MS_MAX;
        const RunResult slow = run_stream(config, constant(4800, 1, 1.0F));
        CHECK(slow.created && slow.processed && slow.flushed);
        CHECK(max_abs(slow.output) <= dbfs_to_linear(-1.0) + 1e-6);
        CHECK(slow.values.over_ceiling_samples == 0);
    }
    {
        /* 0 dBFS ceiling is the top of the allowed range and must not limit
         * a signal that is already exactly at full scale. */
        dg_limiter_config config = base_config(48000, 1);
        config.ceiling_dbfs = DG_LIMITER_CEILING_DBFS_MAX;
        config.lookahead_ms = 1.0;
        const RunResult result = run_stream(config, constant(4800, 1, 1.0F));
        CHECK(result.created && result.processed && result.flushed);
        CHECK(max_abs(result.output) <= 1.0 + 1e-6);
        CHECK(result.values.over_ceiling_samples == 0);
        CHECK(result.values.limited_samples == 0);
    }
    {
        /* 20 ms at 192 kHz is the maximum lookahead: 3840 frames. */
        dg_limiter_config config = base_config(192000, 2);
        config.lookahead_ms = DG_LIMITER_LOOKAHEAD_MS_MAX;
        config.ceiling_dbfs = 0.0;
        dg_limiter* limiter = nullptr;
        EXPECT_STATUS(dg_limiter_create(&config, &limiter), DG_OK);
        if (limiter != nullptr) {
            CHECK(dg_limiter_latency_frames(limiter) == 3840);
            EXPECT_STATUS(dg_limiter_destroy(limiter), DG_OK);
        } else {
            CHECK(false);
        }
    }
    {
        /* Silence must not limit, and the output must be exactly silent. */
        dg_limiter_config config = base_config(48000, 2);
        const RunResult result = run_stream(config, constant(4800, 2, 0.0F));
        CHECK(result.created && result.processed && result.flushed);
        CHECK(max_abs(result.output) == 0.0);
        CHECK(result.values.limited_samples == 0);
        CHECK_CLOSE(result.values.current_gain_db, 0.0, 1e-9);
    }
    {
        /* A full-scale impulse on the very last frame has to survive the flush
         * without exceeding the ceiling. */
        dg_limiter_config config = base_config(48000, 1);
        config.ceiling_dbfs = -3.0;
        config.lookahead_ms = 5.0;
        std::vector<float> input(512, 0.0F);
        input.back() = 1.0F;
        const RunResult result = run_stream(config, input);
        CHECK(result.created && result.processed && result.flushed);
        CHECK_CLOSE(result.output.back(), dbfs_to_linear(-3.0), 1e-6);
        CHECK(max_abs(result.output) <= dbfs_to_linear(-3.0) + 1e-6);
    }
}

void test_true_peak_gap_evidence() {
    /* The sample-peak limiter only constrains x[n]. A sine at fs/4 with a
     * 45-degree phase has every sample at A/sqrt(2) but its reconstructed peak
     * at A. With A = 1.2 and a -2 dBFS ceiling the limiter reduces the samples
     * to the ceiling (about 0.794) but the reconstructed waveform still peaks
     * near 1.123, i.e. above 0 dBFS. This is the DG-014 gap, measured with the
     * new analyser; the limiter itself is unchanged. */
    dg_limiter_config config = base_config(48000, 1);
    config.ceiling_dbfs = -2.0;
    config.lookahead_ms = 2.0;
    config.release_ms = 50.0;

    const std::size_t frames = 9600;
    const std::size_t fade = 480;
    std::vector<float> input(frames, 0.0F);
    for (std::size_t n = 0; n < frames; ++n) {
        double envelope = 1.0;
        if (n < fade) {
            /* Smooth fade-in so the measurement is not dominated by a step. */
            envelope = 0.5 - 0.5 * std::cos(kPi * static_cast<double>(n) /
                                            static_cast<double>(fade));
        }
        input[n] = static_cast<float>(
            1.2 * envelope * std::sin(kPi * static_cast<double>(n) / 2.0 + kPi / 4.0));
    }

    const RunResult result = run_stream(config, input);
    CHECK(result.created && result.processed && result.flushed);
    if (!result.processed) {
        return;
    }
    const double ceiling = dbfs_to_linear(config.ceiling_dbfs);
    CHECK(result.values.over_ceiling_samples == 0);
    CHECK(result.values.limited_samples > 0);
    CHECK(max_abs(result.output) <= ceiling + 1e-6);

    dg_true_peak_config analyzer_config{};
    analyzer_config.sample_rate = 48000;
    analyzer_config.channel_count = 1;
    dg_true_peak* analyzer = nullptr;
    EXPECT_STATUS(dg_true_peak_create(&analyzer_config, &analyzer), DG_OK);
    if (analyzer == nullptr) {
        CHECK(false);
        return;
    }
    EXPECT_STATUS(dg_true_peak_process(analyzer, view_of(result.output, 1)), DG_OK);
    dg_true_peak_values measured{};
    EXPECT_STATUS(dg_true_peak_snapshot(analyzer, &measured), DG_OK);
    EXPECT_STATUS(dg_true_peak_destroy(analyzer), DG_OK);

    /* The sample-peak bound still holds, yet the inter-sample peak is far above
     * the ceiling and even above digital full scale. */
    CHECK(measured.total_sample_peak_linear <= ceiling + 1e-6);
    CHECK(measured.total_sample_peak_linear >= ceiling - 1e-4);
    CHECK(measured.total_true_peak_linear > ceiling * 1.25);
    CHECK(measured.total_true_peak_dbfs > 0.0);
    std::printf(
        "  true-peak gap: ceiling %.3f dBFS, output sample peak %.3f dBFS, "
        "true peak %.3f dBFS, overshoot %.2f dB\n",
        config.ceiling_dbfs, measured.total_sample_peak_dbfs,
        measured.total_true_peak_dbfs,
        measured.total_true_peak_dbfs - config.ceiling_dbfs);
}

void test_rate_and_channel_smoke() {
    const int32_t rates[] = {8000, 44100, 48000, 96000, 192000};
    for (int32_t rate : rates) {
        for (int32_t channels = 1; channels <= 2; ++channels) {
            dg_limiter_config config = base_config(rate, channels);
            config.lookahead_ms = 5.0;
            config.release_ms = 10.0;
            const std::size_t frames = static_cast<std::size_t>(rate) / 10U;
            const RunResult result = run_stream(config, constant(frames, channels, 1.0F));
            CHECK(result.created && result.processed && result.flushed);
            CHECK(result.values.frames == frames);
            CHECK(result.values.limited_samples > 0);
            CHECK(max_abs(result.output) <= dbfs_to_linear(config.ceiling_dbfs) + 1e-6);
            CHECK(std::isfinite(result.values.input_peak_db));
            CHECK(std::isfinite(result.values.output_peak_db));
        }
    }
}

void run(const char* name, void (*function)()) {
    const int before = g_failures;
    function();
    std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name);
}

}  // namespace

int main() {
    std::printf("dg_limiter tests (DG-014, sample peak only; no true-peak measurement)\n");

    run("create validation", test_create_validation);
    run("null handles and descriptor validation", test_null_and_descriptor_validation);
    run("latency and pass-through", test_latency_and_passthrough);
    run("impulse latency", test_impulse_latency);
    run("sample-peak bound across rates, ceilings and lookaheads", test_sample_peak_bound);
    run("stereo link", test_stereo_link);
    run("chunking invariance", test_chunking_invariance);
    run("full window with increasing required gain", test_full_window_increasing_required_gain);
    run("flush tail", test_flush_tail);
    run("reset", test_reset);
    run("in-place aliasing", test_in_place);
    run("non-finite input", test_non_finite_input);
    run("extremes", test_extremes);
    run("rate and channel smoke", test_rate_and_channel_smoke);
    run("true-peak gap evidence (measurement only)", test_true_peak_gap_evidence);

    std::printf("  %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
