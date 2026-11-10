// Tests for the streaming Float32 PCM meter (DG-012).
//
// Run on the host through CTest, same style as the other test programs: no
// framework, one process, a non-zero exit code on failure.
//
// What these tests establish:
//   * descriptor validation (NULL config/handle, out-of-range rate, channels and
//     window, mismatched channel count) and the documented status codes;
//   * zero-input and word-boundary behaviour: exactly-zero streams report RMS 0,
//     peak 0 and -INFINITY dBFS, while |x| == 1.0 is not a clip and NaN/Inf are
//     counted but excluded from energy, peak and the rolling window;
//   * known-signal arithmetic on full-scale DC, a calibrated 1 kHz sine and a
//     stereo signal with different channel levels;
//   * rolling-window and total semantics across the wrap point of the ring;
//   * chunking: any split of the same stream produces bit-identical values;
//   * reset() and reset_peak_hold() clear exactly what they document.
//
// What these tests do NOT establish: no capture device is involved, nothing is
// wired into playback, and this is not a leak or race detector. Meter values are
// sample peak and RMS only; true-peak is DG-014.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "dreamguard/meter.h"
#include "dreamguard/pcm.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void expect(bool condition, const char* expression, int line) {
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_failures;
    std::printf("    FAIL line %d: %s\n", line, expression);
}

void expect_status(const char* call, dg_status actual, dg_status expected, int line) {
    ++g_checks;
    if (actual == expected) {
        return;
    }
    ++g_failures;
    std::printf("    FAIL line %d: %s returned %d, expected %d\n", line, call,
                static_cast<int>(actual), static_cast<int>(expected));
}

#define CHECK(expression) expect((expression), #expression, __LINE__)
#define EXPECT_STATUS(call, expected) \
    expect_status(#call, (call), (expected), __LINE__)

void run(const char* name, void (*test)()) {
    std::printf("  %s\n", name);
    test();
}

bool near(double actual, double expected, double tolerance) {
    return std::fabs(actual - expected) <= tolerance;
}

dg_meter_config make_config(int32_t rate, int32_t channels, int32_t window_ms) {
    dg_meter_config config{};
    config.sample_rate = rate;
    config.channel_count = channels;
    config.window_ms = window_ms;
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

// A pure tone whose channel amplitude is a per-channel constant factor.
std::vector<float> tone(std::size_t frames, int32_t channels, double rate,
                        double frequency, double left_amplitude,
                        double right_amplitude) {
    std::vector<float> samples(frames * static_cast<std::size_t>(channels), 0.0F);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double phase =
            2.0 * 3.14159265358979323846 * frequency *
            static_cast<double>(frame) / rate;
        const double value = std::sin(phase);
        samples[frame * static_cast<std::size_t>(channels)] =
            static_cast<float>(left_amplitude * value);
        if (channels == 2) {
            samples[frame * static_cast<std::size_t>(channels) + 1] =
                static_cast<float>(right_amplitude * value);
        }
    }
    return samples;
}

// Deterministic pseudo-random material in [-1, 1), one call to fill any length.
std::vector<float> noise(std::size_t frames, int32_t channels, std::uint32_t seed) {
    std::vector<float> samples(frames * static_cast<std::size_t>(channels), 0.0F);
    std::uint32_t state = seed;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        state = state * 1664525U + 1013904223U;
        const float unit =
            static_cast<float>(state >> 8) / 8388608.0F; /* [0, 2) */
        samples[i] = unit - 1.0F;
    }
    return samples;
}

dg_meter_values snapshot(dg_meter* meter) {
    dg_meter_values values{};
    EXPECT_STATUS(dg_meter_snapshot(meter, &values), DG_OK);
    return values;
}

void test_create_validation() {
    dg_meter* meter = reinterpret_cast<dg_meter*>(static_cast<std::uintptr_t>(1));
    EXPECT_STATUS(dg_meter_create(nullptr, &meter), DG_ERR_INVALID_ARGUMENT);
    CHECK(meter == nullptr);

    dg_meter_config config = make_config(48000, 1, 0);
    EXPECT_STATUS(dg_meter_create(&config, nullptr), DG_ERR_INVALID_ARGUMENT);

    config = make_config(7999, 1, 0);
    meter = reinterpret_cast<dg_meter*>(static_cast<std::uintptr_t>(1));
    EXPECT_STATUS(dg_meter_create(&config, &meter), DG_ERR_SAMPLE_RATE_OUT_OF_RANGE);
    CHECK(meter == nullptr);

    config = make_config(192001, 1, 0);
    EXPECT_STATUS(dg_meter_create(&config, &meter), DG_ERR_SAMPLE_RATE_OUT_OF_RANGE);

    config = make_config(48000, 0, 0);
    EXPECT_STATUS(dg_meter_create(&config, &meter), DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE);

    config = make_config(48000, 3, 0);
    EXPECT_STATUS(dg_meter_create(&config, &meter), DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE);

    config = make_config(48000, 1, -1);
    EXPECT_STATUS(dg_meter_create(&config, &meter), DG_ERR_INVALID_ARGUMENT);

    config = make_config(48000, 1, DG_METER_WINDOW_MS_MAX + 1);
    EXPECT_STATUS(dg_meter_create(&config, &meter), DG_ERR_INVALID_ARGUMENT);

    /* 0 selects the documented default; the low and high window bounds work. */
    config = make_config(48000, 2, 0);
    EXPECT_STATUS(dg_meter_create(&config, &meter), DG_OK);
    CHECK(meter != nullptr);
    if (meter != nullptr) {
        dg_meter_values values = snapshot(meter);
        CHECK(values.window_capacity_frames == 19200U);
        EXPECT_STATUS(dg_meter_destroy(meter), DG_OK);
    }

    config = make_config(8000, 1, DG_METER_WINDOW_MS_MIN);
    EXPECT_STATUS(dg_meter_create(&config, &meter), DG_OK);
    if (meter != nullptr) {
        CHECK(snapshot(meter).window_capacity_frames == 8U);
        EXPECT_STATUS(dg_meter_destroy(meter), DG_OK);
    }

    config = make_config(192000, 2, DG_METER_WINDOW_MS_MAX);
    EXPECT_STATUS(dg_meter_create(&config, &meter), DG_OK);
    if (meter != nullptr) {
        CHECK(snapshot(meter).window_capacity_frames == 192000U);
        EXPECT_STATUS(dg_meter_destroy(meter), DG_OK);
    }
}

void test_null_handle_and_fresh_state() {
    dg_meter_values values{};
    EXPECT_STATUS(dg_meter_destroy(nullptr), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_meter_reset(nullptr), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_meter_reset_peak_hold(nullptr), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_meter_snapshot(nullptr, &values), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_meter_process(nullptr, dg_pcm_view{nullptr, 0, 1}),
                  DG_ERR_NULL_HANDLE);

    dg_meter_config config = make_config(48000, 2, 0);
    dg_meter* meter = nullptr;
    EXPECT_STATUS(dg_meter_create(&config, &meter), DG_OK);
    CHECK(meter != nullptr);
    if (meter == nullptr) {
        return;
    }
    dg_meter_values fresh = snapshot(meter);
    CHECK(fresh.total_frames == 0U);
    CHECK(fresh.clip_samples == 0U);
    CHECK(fresh.non_finite_samples == 0U);
    CHECK(fresh.window_fill_frames == 0U);
    CHECK(fresh.window_capacity_frames == 19200U);
    for (int ch = 0; ch < 2; ++ch) {
        CHECK(fresh.block_peak[ch] == 0.0F);
        CHECK(fresh.block_rms[ch] == 0.0F);
        CHECK(fresh.window_peak[ch] == 0.0F);
        CHECK(fresh.window_rms[ch] == 0.0F);
        CHECK(fresh.peak_hold[ch] == 0.0F);
        CHECK(fresh.total_rms[ch] == 0.0F);
    }
    EXPECT_STATUS(dg_meter_destroy(meter), DG_OK);
}

void test_rejections_are_atomic() {
    dg_meter_config config = make_config(48000, 2, 0);
    dg_meter* meter = nullptr;
    EXPECT_STATUS(dg_meter_create(&config, &meter), DG_OK);
    if (meter == nullptr) {
        return;
    }

    std::vector<float> mono_samples = tone(480, 1, 48000.0, 1000.0, 0.25, 0.0);
    const dg_meter_values before = snapshot(meter);

    dg_pcm_view wrong_channels = view_of(mono_samples, 1);
    EXPECT_STATUS(dg_meter_process(meter, wrong_channels), DG_ERR_INVALID_ARGUMENT);
    dg_pcm_view null_samples{nullptr, 480, 2};
    EXPECT_STATUS(dg_meter_process(meter, null_samples), DG_ERR_INVALID_ARGUMENT);
    dg_pcm_view three_channels{mono_samples.data(), 480, 3};
    EXPECT_STATUS(dg_meter_process(meter, three_channels), DG_ERR_INVALID_ARGUMENT);

    const dg_meter_values after = snapshot(meter);
    CHECK(after.total_frames == before.total_frames);
    CHECK(after.window_fill_frames == before.window_fill_frames);
    CHECK(after.block_rms[0] == before.block_rms[0]);
    CHECK(after.block_peak[1] == before.block_peak[1]);

    /* A zero-frame view is accepted and must not clear block_* values... but the
     * meter starts empty here, so first give it a real block. */
    std::vector<float> stereo = tone(480, 2, 48000.0, 1000.0, 0.5, 0.25);
    EXPECT_STATUS(dg_meter_process(meter, view_of(stereo, 2)), DG_OK);
    const dg_meter_values with_block = snapshot(meter);
    CHECK(with_block.block_rms[0] > 0.0F);

    dg_pcm_view empty{nullptr, 0, 2};
    EXPECT_STATUS(dg_meter_process(meter, empty), DG_OK);
    const dg_meter_values after_empty = snapshot(meter);
    CHECK(after_empty.total_frames == with_block.total_frames);
    CHECK(after_empty.block_rms[0] == with_block.block_rms[0]);
    CHECK(after_empty.block_peak[1] == with_block.block_peak[1]);

    EXPECT_STATUS(dg_meter_destroy(meter), DG_OK);
}

void test_dc_and_sine() {
    dg_meter_config config = make_config(48000, 1, 0);
    dg_meter* meter = nullptr;
    EXPECT_STATUS(dg_meter_create(&config, &meter), DG_OK);
    if (meter == nullptr) {
        return;
    }

    /* Full-scale DC: RMS 1, peak 1, 0 dBFS, and exactly 1.0 is not a clip. */
    std::vector<float> dc(4800, 1.0F);
    EXPECT_STATUS(dg_meter_process(meter, view_of(dc, 1)), DG_OK);
    dg_meter_values values = snapshot(meter);
    CHECK(values.block_rms[0] == 1.0F);
    CHECK(values.block_peak[0] == 1.0F);
    CHECK(values.window_rms[0] == 1.0F);
    CHECK(values.peak_hold[0] == 1.0F);
    CHECK(values.clip_samples == 0U);
    CHECK(dg_meter_linear_to_dbfs(values.block_rms[0]) == 0.0F);

    /* -0.0 is silence for metering purposes. */
    std::vector<float> negative_zero(480, -0.0F);
    EXPECT_STATUS(dg_meter_reset(meter), DG_OK);
    EXPECT_STATUS(dg_meter_process(meter, view_of(negative_zero, 1)), DG_OK);
    values = snapshot(meter);
    CHECK(values.block_rms[0] == 0.0F);
    CHECK(values.block_peak[0] == 0.0F);
    CHECK(std::isinf(dg_meter_linear_to_dbfs(values.block_rms[0])));

    /* 1 kHz sine, peak 0.5, exactly 100 cycles: RMS = 0.5/sqrt(2), peak = 0.5. */
    EXPECT_STATUS(dg_meter_reset(meter), DG_OK);
    std::vector<float> sine = tone(4800, 1, 48000.0, 1000.0, 0.5, 0.0);
    EXPECT_STATUS(dg_meter_process(meter, view_of(sine, 1)), DG_OK);
    values = snapshot(meter);
    CHECK(near(values.block_rms[0], 0.5 / std::sqrt(2.0), 1e-4));
    CHECK(near(values.block_peak[0], 0.5, 1e-6));
    CHECK(near(values.total_rms[0], 0.5 / std::sqrt(2.0), 1e-4));
    CHECK(near(dg_meter_linear_to_dbfs(values.block_rms[0]), -9.0309, 1e-3));

    EXPECT_STATUS(dg_meter_destroy(meter), DG_OK);
}

void test_stereo_channels_are_independent() {
    dg_meter_config config = make_config(48000, 2, 0);
    dg_meter* meter = nullptr;
    EXPECT_STATUS(dg_meter_create(&config, &meter), DG_OK);
    if (meter == nullptr) {
        return;
    }
    std::vector<float> sine = tone(4800, 2, 48000.0, 1000.0, 0.5, 0.25);
    EXPECT_STATUS(dg_meter_process(meter, view_of(sine, 2)), DG_OK);
    const dg_meter_values values = snapshot(meter);
    CHECK(near(values.block_rms[0], 0.5 / std::sqrt(2.0), 1e-4));
    CHECK(near(values.block_rms[1], 0.25 / std::sqrt(2.0), 1e-4));
    CHECK(near(values.block_peak[0], 0.5, 1e-6));
    CHECK(near(values.block_peak[1], 0.25, 1e-6));
    CHECK(values.total_frames == 4800U);
    EXPECT_STATUS(dg_meter_destroy(meter), DG_OK);
}

void test_window_and_total() {
    /* 40 ms window at 48 kHz = 1920 frames. Three 480-frame blocks of 1.0 fill
     * three quarters of the window; one block of 0.0 then leaves three quarters
     * of the window at full scale, and the total RMS over the same 1920 frames
     * matches. The window peak only drops once every full-scale frame has left
     * the ring. */
    dg_meter_config config = make_config(48000, 1, 40);
    dg_meter* meter = nullptr;
    EXPECT_STATUS(dg_meter_create(&config, &meter), DG_OK);
    if (meter == nullptr) {
        return;
    }
    std::vector<float> ones(480, 1.0F);
    for (int i = 0; i < 3; ++i) {
        EXPECT_STATUS(dg_meter_process(meter, view_of(ones, 1)), DG_OK);
    }
    dg_meter_values values = snapshot(meter);
    CHECK(values.window_capacity_frames == 1920U);
    CHECK(values.window_fill_frames == 1440U);
    CHECK(values.window_rms[0] == 1.0F);
    CHECK(values.window_peak[0] == 1.0F);
    CHECK(values.total_frames == 1440U);
    CHECK(values.total_rms[0] == 1.0F);

    std::vector<float> zeros(480, 0.0F);
    EXPECT_STATUS(dg_meter_process(meter, view_of(zeros, 1)), DG_OK);
    values = snapshot(meter);
    CHECK(values.window_fill_frames == 1920U);
    CHECK(near(values.window_rms[0], std::sqrt(0.75), 1e-6));
    CHECK(values.window_peak[0] == 1.0F);
    CHECK(values.block_rms[0] == 0.0F);
    CHECK(near(values.total_rms[0], std::sqrt(0.75), 1e-6));

    /* Four more silent blocks push every full-scale frame out of the ring. */
    for (int i = 0; i < 4; ++i) {
        EXPECT_STATUS(dg_meter_process(meter, view_of(zeros, 1)), DG_OK);
    }
    values = snapshot(meter);
    CHECK(values.window_peak[0] == 0.0F);
    CHECK(values.window_rms[0] == 0.0F);

    EXPECT_STATUS(dg_meter_destroy(meter), DG_OK);
}

void test_clip_and_non_finite_counting() {
    dg_meter_config config = make_config(48000, 1, 0);
    dg_meter* meter = nullptr;
    EXPECT_STATUS(dg_meter_create(&config, &meter), DG_OK);
    if (meter == nullptr) {
        return;
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> samples{1.0F, 1.5F, -2.0F, inf, nan, -inf};
    EXPECT_STATUS(dg_meter_process(meter, view_of(samples, 1)), DG_OK);
    const dg_meter_values values = snapshot(meter);
    CHECK(values.clip_samples == 2U);
    CHECK(values.non_finite_samples == 3U);
    /* Energy from the finite samples: 1 + 2.25 + 4 = 7.25 over six frames. */
    CHECK(near(values.block_rms[0], std::sqrt(7.25 / 6.0), 1e-6));
    CHECK(near(values.block_peak[0], 2.0, 1e-6));
    CHECK(values.total_frames == 6U);

    /* The smallest positive denormal must survive: the square is taken in double
     * (where (1.4e-45)^2 ~ 2e-90 is still representable), so the RMS reads the
     * sample back instead of silently underflowing to zero. */
    EXPECT_STATUS(dg_meter_reset(meter), DG_OK);
    const float denormal = std::numeric_limits<float>::denorm_min();
    std::vector<float> tiny(4, denormal);
    EXPECT_STATUS(dg_meter_process(meter, view_of(tiny, 1)), DG_OK);
    const dg_meter_values tiny_values = snapshot(meter);
    CHECK(tiny_values.block_peak[0] == denormal);
    CHECK(tiny_values.window_peak[0] == denormal);
    CHECK(tiny_values.block_rms[0] == denormal);
    CHECK(tiny_values.total_rms[0] == denormal);

    EXPECT_STATUS(dg_meter_destroy(meter), DG_OK);
}

void test_chunking_is_bit_identical() {
    const std::size_t frames = 48000;
    const int32_t channels = 2;
    const std::vector<float> whole = noise(frames, channels, 0xC0FFEEU);

    dg_meter_config config = make_config(48000, channels, 0);
    dg_meter* one_shot = nullptr;
    dg_meter* chunked = nullptr;
    EXPECT_STATUS(dg_meter_create(&config, &one_shot), DG_OK);
    EXPECT_STATUS(dg_meter_create(&config, &chunked), DG_OK);
    if (one_shot == nullptr || chunked == nullptr) {
        return;
    }
    EXPECT_STATUS(dg_meter_process(one_shot, view_of(whole, channels)), DG_OK);

    const std::size_t chunk_sizes[] = {1, 7, 480, 1000, 4801, 96, 23999};
    std::size_t offset = 0;
    std::size_t chunk_index = 0;
    while (offset < frames) {
        std::size_t take = chunk_sizes[chunk_index % 7];
        if (take > frames - offset) {
            take = frames - offset;
        }
        dg_pcm_view part{};
        part.samples = whole.data() + offset * static_cast<std::size_t>(channels);
        part.frame_count = static_cast<uint32_t>(take);
        part.channel_count = channels;
        EXPECT_STATUS(dg_meter_process(chunked, part), DG_OK);
        offset += take;
        ++chunk_index;
    }

    const dg_meter_values a = snapshot(one_shot);
    const dg_meter_values b = snapshot(chunked);
    /* block_* describes the most recent call, so it is intentionally NOT part of
     * this comparison: one-shot sees all 48000 frames, the chunked run sees the
     * last partial chunk. Everything that is defined over the stream must agree
     * bit for bit, because the per-frame work happens in the same order. */
    CHECK(a.total_frames == b.total_frames);
    CHECK(a.clip_samples == b.clip_samples);
    CHECK(a.non_finite_samples == b.non_finite_samples);
    CHECK(a.window_fill_frames == b.window_fill_frames);
    CHECK(a.window_capacity_frames == b.window_capacity_frames);
    for (int ch = 0; ch < channels; ++ch) {
        CHECK(a.window_peak[ch] == b.window_peak[ch]);
        CHECK(a.window_rms[ch] == b.window_rms[ch]);
        CHECK(a.peak_hold[ch] == b.peak_hold[ch]);
        CHECK(a.total_rms[ch] == b.total_rms[ch]);
    }
    EXPECT_STATUS(dg_meter_destroy(one_shot), DG_OK);
    EXPECT_STATUS(dg_meter_destroy(chunked), DG_OK);
}

void test_reset_and_peak_hold() {
    dg_meter_config config = make_config(48000, 1, 0);
    dg_meter* meter = nullptr;
    EXPECT_STATUS(dg_meter_create(&config, &meter), DG_OK);
    if (meter == nullptr) {
        return;
    }
    std::vector<float> loud(480, 0.9F);
    EXPECT_STATUS(dg_meter_process(meter, view_of(loud, 1)), DG_OK);
    std::vector<float> quiet(480, 0.1F);
    EXPECT_STATUS(dg_meter_process(meter, view_of(quiet, 1)), DG_OK);
    dg_meter_values values = snapshot(meter);
    CHECK(near(values.block_peak[0], 0.1, 1e-6));
    CHECK(near(values.peak_hold[0], 0.9, 1e-6));

    EXPECT_STATUS(dg_meter_reset_peak_hold(meter), DG_OK);
    values = snapshot(meter);
    CHECK(values.peak_hold[0] == 0.0F);
    CHECK(near(values.block_peak[0], 0.1, 1e-6)); /* block is untouched */
    CHECK(values.total_frames == 960U);           /* totals are untouched */
    CHECK(values.window_fill_frames == 960U);     /* window is untouched */

    EXPECT_STATUS(dg_meter_reset(meter), DG_OK);
    values = snapshot(meter);
    CHECK(values.total_frames == 0U);
    CHECK(values.window_fill_frames == 0U);
    CHECK(values.window_capacity_frames == 19200U);
    CHECK(values.block_rms[0] == 0.0F);
    CHECK(values.block_peak[0] == 0.0F);
    CHECK(values.peak_hold[0] == 0.0F);
    CHECK(values.total_rms[0] == 0.0F);
    EXPECT_STATUS(dg_meter_destroy(meter), DG_OK);
}

void test_linear_to_dbfs() {
    const float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(dg_meter_linear_to_dbfs(1.0F) == 0.0F);
    CHECK(near(dg_meter_linear_to_dbfs(0.5F), -6.0206, 1e-3));
    CHECK(near(dg_meter_linear_to_dbfs(0.1F), -20.0, 1e-3));
    CHECK(std::isinf(dg_meter_linear_to_dbfs(0.0F)));
    CHECK(dg_meter_linear_to_dbfs(0.0F) < 0.0F);
    CHECK(std::isinf(dg_meter_linear_to_dbfs(-0.0F)));
    CHECK(std::isinf(dg_meter_linear_to_dbfs(-1.0F)));
    CHECK(dg_meter_linear_to_dbfs(infinity) == infinity);
    CHECK(std::isnan(dg_meter_linear_to_dbfs(nan)));
}

}  // namespace

int main() {
    std::printf("dg_meter tests\n");
    run("create validation", test_create_validation);
    run("null handle and fresh state", test_null_handle_and_fresh_state);
    run("rejections are atomic", test_rejections_are_atomic);
    run("dc and sine", test_dc_and_sine);
    run("stereo channels are independent", test_stereo_channels_are_independent);
    run("window and total", test_window_and_total);
    run("clip and non-finite counting", test_clip_and_non_finite_counting);
    run("chunking is bit-identical", test_chunking_is_bit_identical);
    run("reset and peak hold", test_reset_and_peak_hold);
    run("linear to dbfs", test_linear_to_dbfs);
    std::printf("  %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
