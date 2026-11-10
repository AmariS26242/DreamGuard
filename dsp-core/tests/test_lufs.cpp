// Tests for the LUFS analyser (DG-012).
//
// Run on the host through CTest, same style as the other test programs.
//
// What these tests establish:
//   * the K-weighting design: at 48 kHz the bilinear-transform coefficients
//     reproduce the published BS.1770-4 table to 1e-6, and stay stable at every
//     supported rate;
//   * EBU R128 / BS.1770-4 calibration conventions: a stereo 1 kHz sine reads its
//     peak level in dBFS within 0.1 LU, and a mono sine of the same amplitude
//     reads 10*log10(2) = 3.0103 LU lower;
//   * window availability: momentary needs four complete 100 ms hops, short-term
//     thirty, and a partial hop is reported through pending_frames instead of
//     being analysed;
//   * gating: absolute silence is -INFINITY, and a -50 LUFS section is excluded by
//     the relative gate while a -20 LUFS section survives;
//   * non-finite samples are counted and cannot poison the recursive filter;
//   * chunking: any split of the same stream produces bit-identical results;
//   * descriptor validation and NULL-handle behaviour.
//
// What these tests do NOT establish: no reference implementation (ffmpeg
// ebur128, libebur128, a broadcast meter) has been run against this code, so the
// component stays PROVISIONAL and no output is claimed to be BS.1770-4
// compliant. There is no sample-rate conversion, no surround or LFE weighting,
// and no true-peak measurement here.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "dreamguard/lufs.h"
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

dg_lufs_config make_config(int32_t rate, int32_t channels) {
    dg_lufs_config config{};
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

dg_lufs_values snapshot(dg_lufs* lufs) {
    dg_lufs_values values{};
    EXPECT_STATUS(dg_lufs_snapshot(lufs, &values), DG_OK);
    return values;
}

/* The published BS.1770-4 48 kHz K-weighting coefficients. */
constexpr double kShelfB0 = 1.53512485958697;
constexpr double kShelfB1 = -2.69169618940638;
constexpr double kShelfB2 = 1.19839281085285;
constexpr double kShelfA1 = -1.69065929318241;
constexpr double kShelfA2 = 0.73248077421585;
constexpr double kHighPassA1 = -1.99004745483398;
constexpr double kHighPassA2 = 0.99007225036621;

void test_create_validation() {
    dg_lufs* lufs = reinterpret_cast<dg_lufs*>(static_cast<std::uintptr_t>(1));
    EXPECT_STATUS(dg_lufs_create(nullptr, &lufs), DG_ERR_INVALID_ARGUMENT);
    CHECK(lufs == nullptr);

    dg_lufs_config config = make_config(48000, 1);
    EXPECT_STATUS(dg_lufs_create(&config, nullptr), DG_ERR_INVALID_ARGUMENT);

    config = make_config(7999, 1);
    lufs = reinterpret_cast<dg_lufs*>(static_cast<std::uintptr_t>(1));
    EXPECT_STATUS(dg_lufs_create(&config, &lufs), DG_ERR_SAMPLE_RATE_OUT_OF_RANGE);
    CHECK(lufs == nullptr);

    config = make_config(192001, 1);
    EXPECT_STATUS(dg_lufs_create(&config, &lufs), DG_ERR_SAMPLE_RATE_OUT_OF_RANGE);

    config = make_config(48000, 0);
    EXPECT_STATUS(dg_lufs_create(&config, &lufs), DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE);

    config = make_config(48000, 3);
    EXPECT_STATUS(dg_lufs_create(&config, &lufs), DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE);

    dg_lufs_k_weighting weighting{};
    EXPECT_STATUS(dg_lufs_k_weighting_for_rate(48000, nullptr),
                  DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_lufs_k_weighting_for_rate(7999, &weighting),
                  DG_ERR_SAMPLE_RATE_OUT_OF_RANGE);
    EXPECT_STATUS(dg_lufs_k_weighting_for_rate(96000, &weighting), DG_OK);
}

void test_null_handles() {
    dg_lufs_values values{};
    EXPECT_STATUS(dg_lufs_destroy(nullptr), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_lufs_reset(nullptr), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_lufs_snapshot(nullptr, &values), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_lufs_process(nullptr, dg_pcm_view{nullptr, 0, 1}),
                  DG_ERR_NULL_HANDLE);
}

void test_k_weighting_matches_published_table() {
    dg_lufs_k_weighting weighting{};
    EXPECT_STATUS(dg_lufs_k_weighting_for_rate(48000, &weighting), DG_OK);
    CHECK(near(weighting.shelf.b0, kShelfB0, 1e-6));
    CHECK(near(weighting.shelf.b1, kShelfB1, 1e-6));
    CHECK(near(weighting.shelf.b2, kShelfB2, 1e-6));
    CHECK(near(weighting.shelf.a1, kShelfA1, 1e-6));
    CHECK(near(weighting.shelf.a2, kShelfA2, 1e-6));
    CHECK(weighting.high_pass.b0 == 1.0);
    CHECK(weighting.high_pass.b1 == -2.0);
    CHECK(weighting.high_pass.b2 == 1.0);
    CHECK(near(weighting.high_pass.a1, kHighPassA1, 1e-6));
    CHECK(near(weighting.high_pass.a2, kHighPassA2, 1e-6));

    /* Every supported rate must yield stable, monotone-in-nothing coefficients:
     * the poles stay inside the unit circle (|a2| < 1, -2 < a1 < 0). */
    const int32_t rates[] = {8000, 11025, 22050, 44100, 48000, 88200, 96000, 192000};
    for (int32_t rate : rates) {
        dg_lufs_k_weighting w{};
        EXPECT_STATUS(dg_lufs_k_weighting_for_rate(rate, &w), DG_OK);
        CHECK(w.shelf.a2 > 0.0 && w.shelf.a2 < 1.0);
        CHECK(w.shelf.a1 > -2.0 && w.shelf.a1 < 0.0);
        CHECK(w.high_pass.a2 > 0.0 && w.high_pass.a2 < 1.0);
        CHECK(w.high_pass.a1 > -2.0 && w.high_pass.a1 < 0.0);
        CHECK(w.shelf.b0 > 0.0);
        CHECK(w.shelf.b2 > 0.0);
    }
}

void test_stereo_calibration_and_channel_sum() {
    const std::size_t frames = 48000 * 4;
    const int32_t channels = 2;
    const double peak = 0.5;
    const std::vector<float> sine = tone(frames, channels, 48000.0, 1000.0, peak, peak);

    dg_lufs* lufs = nullptr;
    dg_lufs_config config = make_config(48000, channels);
    EXPECT_STATUS(dg_lufs_create(&config, &lufs), DG_OK);
    if (lufs == nullptr) {
        return;
    }
    EXPECT_STATUS(dg_lufs_process(lufs, view_of(sine, channels)), DG_OK);
    const dg_lufs_values values = snapshot(lufs);

    /* EBU Tech 3341 convention: a 1 kHz sine reads its peak level in dBFS, to
     * within the standard's 0.1 LU tolerance. The measured design deviation at
     * 1 kHz is about 0.024 LU (the K-weighting response is ~+0.67 dB there while
     * the offset is 0.691 dB); it is recorded in docs/STATUS.md rather than
     * hidden by a wider tolerance. */
    const double peak_dbfs = 20.0 * std::log10(peak);
    CHECK(std::isfinite(values.momentary_lufs));
    CHECK(std::isfinite(values.short_term_lufs));
    CHECK(values.integrated_ready == 1);
    CHECK(near(values.integrated_lufs, peak_dbfs, 0.1));
    CHECK(near(values.momentary_lufs, values.integrated_lufs, 0.01));
    CHECK(near(values.short_term_lufs, values.integrated_lufs, 0.01));
    CHECK(values.blocks > 30U);
    CHECK(values.blocks_used == values.blocks_after_absolute_gate);
    CHECK(values.hop_frames == 4800U);
    CHECK(values.block_frames == 19200U);
    CHECK(values.pending_frames == 0U);
    EXPECT_STATUS(dg_lufs_destroy(lufs), DG_OK);

    /* Mono at the same amplitude must read 10*log10(2) LU lower, because loudness
     * sums channel power and both channels carry G = 1.0. */
    const int32_t mono_channels = 1;
    const std::vector<float> mono = tone(frames, mono_channels, 48000.0, 1000.0, peak, 0.0);
    dg_lufs* mono_lufs = nullptr;
    dg_lufs_config mono_config = make_config(48000, mono_channels);
    EXPECT_STATUS(dg_lufs_create(&mono_config, &mono_lufs), DG_OK);
    if (mono_lufs == nullptr) {
        return;
    }
    EXPECT_STATUS(dg_lufs_process(mono_lufs, view_of(mono, mono_channels)), DG_OK);
    const dg_lufs_values mono_values = snapshot(mono_lufs);
    CHECK(near(values.integrated_lufs - mono_values.integrated_lufs,
               10.0 * std::log10(2.0), 1e-9));
    CHECK(near(mono_values.integrated_lufs, peak_dbfs - 10.0 * std::log10(2.0), 0.1));
    EXPECT_STATUS(dg_lufs_destroy(mono_lufs), DG_OK);
}

void test_window_availability() {
    dg_lufs* lufs = nullptr;
    dg_lufs_config config = make_config(48000, 1);
    EXPECT_STATUS(dg_lufs_create(&config, &lufs), DG_OK);
    if (lufs == nullptr) {
        return;
    }
    /* 0.35 s = three complete hops plus 2400 pending frames: no 400 ms window yet,
     * and the partial hop is reported instead of analysed. */
    const std::vector<float> tone_350ms = tone(16800, 1, 48000.0, 1000.0, 0.5, 0.0);
    EXPECT_STATUS(dg_lufs_process(lufs, view_of(tone_350ms, 1)), DG_OK);
    dg_lufs_values values = snapshot(lufs);
    CHECK(values.pending_frames == 2400U);
    CHECK(values.blocks == 0U);
    CHECK(values.integrated_ready == 0);
    CHECK(std::isinf(values.momentary_lufs));
    CHECK(values.momentary_lufs < 0.0);
    CHECK(std::isinf(values.short_term_lufs));

    /* 0.5 s in total: five hops, two 400 ms blocks, momentary available. */
    const std::vector<float> tone_150ms = tone(7200, 1, 48000.0, 1000.0, 0.5, 0.0);
    EXPECT_STATUS(dg_lufs_process(lufs, view_of(tone_150ms, 1)), DG_OK);
    values = snapshot(lufs);
    CHECK(values.pending_frames == 0U);
    CHECK(values.blocks == 2U);
    CHECK(values.integrated_ready == 1);
    CHECK(std::isfinite(values.momentary_lufs));
    CHECK(std::isinf(values.short_term_lufs));

    /* 3.2 s in total: short-term becomes available. */
    const std::vector<float> tone_2700ms = tone(129600, 1, 48000.0, 1000.0, 0.5, 0.0);
    EXPECT_STATUS(dg_lufs_process(lufs, view_of(tone_2700ms, 1)), DG_OK);
    values = snapshot(lufs);
    CHECK(std::isfinite(values.short_term_lufs));
    CHECK(near(values.short_term_lufs, values.momentary_lufs, 0.01));
    EXPECT_STATUS(dg_lufs_destroy(lufs), DG_OK);
}

void test_silence_is_minus_infinity() {
    dg_lufs* lufs = nullptr;
    dg_lufs_config config = make_config(48000, 2);
    EXPECT_STATUS(dg_lufs_create(&config, &lufs), DG_OK);
    if (lufs == nullptr) {
        return;
    }
    const std::vector<float> silence(48000 * 2 * 2, 0.0F);
    EXPECT_STATUS(dg_lufs_process(lufs, view_of(silence, 2)), DG_OK);
    const dg_lufs_values values = snapshot(lufs);
    CHECK(values.blocks > 10U);
    CHECK(values.blocks_after_absolute_gate == 0U);
    CHECK(values.blocks_used == 0U);
    CHECK(values.integrated_ready == 0);
    CHECK(std::isinf(values.integrated_lufs));
    CHECK(std::isinf(values.momentary_lufs));
    CHECK(std::isinf(values.short_term_lufs));
    EXPECT_STATUS(dg_lufs_destroy(lufs), DG_OK);
}

void test_gating() {
    dg_lufs* lufs = nullptr;
    dg_lufs_config config = make_config(48000, 1);
    EXPECT_STATUS(dg_lufs_create(&config, &lufs), DG_OK);
    if (lufs == nullptr) {
        return;
    }
    const std::size_t seconds = 48000;
    const std::vector<float> loud = tone(4 * seconds, 1, 48000.0, 1000.0, 0.1, 0.0);
    const std::vector<float> silence(seconds, 0.0F);
    const double quiet_peak = std::pow(10.0, -50.0 / 20.0);
    const std::vector<float> quiet =
        tone(2 * seconds, 1, 48000.0, 1000.0, quiet_peak, 0.0);

    EXPECT_STATUS(dg_lufs_process(lufs, view_of(loud, 1)), DG_OK);
    EXPECT_STATUS(dg_lufs_process(lufs, view_of(silence, 1)), DG_OK);
    EXPECT_STATUS(dg_lufs_process(lufs, view_of(quiet, 1)), DG_OK);

    const dg_lufs_values values = snapshot(lufs);
    std::printf("    gating: blocks=%llu after_abs=%llu used=%llu integrated=%.4f\n",
                static_cast<unsigned long long>(values.blocks),
                static_cast<unsigned long long>(values.blocks_after_absolute_gate),
                static_cast<unsigned long long>(values.blocks_used),
                values.integrated_lufs);

    /* The test tone is a sine, so its loudness is its RMS level, i.e. 10*log10(2)
     * = 3.0103 LU below its peak level in dBFS, and a MONO sine reads 3.0103 LU
     * lower than a stereo sine of the same peak amplitude (power sums over
     * channels, G = 1.0 for both). That is exactly what the mono half of
     * test_stereo_calibration_and_channel_sum() asserts, so a mono 0.1-peak sine
     * must read 20*log10(0.1) - 3.0103 = -23.01 LUFS, not -20. The first version
     * of this test forgot the -3.0103 LU term, which is why the first device run
     * failed on the two lines below (recorded in docs/DEVLOG.md, round 7).
     *
     * The gating side of the test is unchanged and deliberately strict: the
     * quiet section is above the -70 LUFS absolute gate but 30 dB below the loud
     * one, so the relative gate (mean of the absolute-gated blocks, minus 10 LU,
     * i.e. 10x in power) must drop every quiet block. Only the 37 fully loud
     * blocks and the 3 partially-loud 400 ms blocks that straddle the level drop
     * survive, which is why the tolerance is 0.5 LU rather than the meter's
     * 0.1 LU: those transition blocks (75 %, 50 %, 25 % loud) pull the survivor
     * mean about 0.17 LU down. */
    const double peak_dbfs = 20.0 * std::log10(0.1);
    const double mono_offset = 10.0 * std::log10(2.0);
    CHECK(values.integrated_ready == 1);
    CHECK(near(values.integrated_lufs, peak_dbfs - mono_offset, 0.5));
    CHECK(values.integrated_lufs > peak_dbfs - mono_offset - 1.0);
    CHECK(values.integrated_lufs < peak_dbfs - mono_offset + 1.0);

    /* Gate bookkeeping for 7 s of audio: 70 hops, 70 - 3 = 67 overlapping
     * blocks. The fully quiet blocks (17) and the three quiet transition blocks
     * must all fail the relative gate, leaving the 37 loud blocks plus the three
     * loud-to-silence transitions. The absolute-gated count also contains the
     * 400 ms block that starts with the high-pass ringing tail of the loud
     * section, so it is at least 60 (61 if that tail is above -70 LUFS). */
    CHECK(values.blocks == 67U);
    CHECK(values.blocks_after_absolute_gate >= 60U);
    CHECK(values.blocks_after_absolute_gate > values.blocks_used);
    CHECK(values.blocks_used == 40U);
    EXPECT_STATUS(dg_lufs_destroy(lufs), DG_OK);
}

void test_relative_gate_keeps_near_blocks() {
    /* Complement of test_gating(): a section only 9 dB down in power must stay in
     * the integrated value. This pins the other side of the relative gate, so a
     * future change cannot "fix" gating by dropping quiet-ish blocks that the
     * standard keeps. */
    dg_lufs* lufs = nullptr;
    dg_lufs_config config = make_config(48000, 1);
    EXPECT_STATUS(dg_lufs_create(&config, &lufs), DG_OK);
    if (lufs == nullptr) {
        return;
    }
    const std::size_t seconds = 48000;
    const double peak_dbfs = 20.0 * std::log10(0.1);
    const double mono_offset = 10.0 * std::log10(2.0);
    const double quiet_energy = std::pow(10.0, -9.0 / 10.0);
    const double quiet_peak = 0.1 * std::pow(10.0, -9.0 / 20.0);
    const std::vector<float> loud = tone(4 * seconds, 1, 48000.0, 1000.0, 0.1, 0.0);
    const std::vector<float> quiet = tone(2 * seconds, 1, 48000.0, 1000.0, quiet_peak, 0.0);

    EXPECT_STATUS(dg_lufs_process(lufs, view_of(loud, 1)), DG_OK);
    EXPECT_STATUS(dg_lufs_process(lufs, view_of(quiet, 1)), DG_OK);
    const dg_lufs_values values = snapshot(lufs);

    /* Hand-computed block model: 6 s is 60 hops, so 57 overlapping 400 ms blocks.
     * 37 are fully loud (relative energy 1), 17 are fully quiet (0.126) and the 3
     * blocks that straddle the drop hold 0.75/0.5/0.25 of the loud section and
     * 0.25/0.5/0.75 of the quiet one, i.e. 1.5 loud-energy units plus 1.5 quiet
     * units. All 57 pass the -70 LUFS absolute gate; the survivor mean is ~0.716
     * of the loud block energy, and mean/10 = 0.072 stays below every block, so
     * the relative gate must keep all of them and the integrated value is the
     * mean of all 57 blocks. */
    const double expected_mean = (37.0 + 1.5 + (17.0 + 1.5) * quiet_energy) / 57.0;
    const double expected = peak_dbfs - mono_offset + 10.0 * std::log10(expected_mean);
    CHECK(values.blocks == 57U);
    CHECK(values.blocks_after_absolute_gate == 57U);
    CHECK(values.blocks_used == values.blocks_after_absolute_gate);
    CHECK(near(values.integrated_lufs, expected, 0.5));
    EXPECT_STATUS(dg_lufs_destroy(lufs), DG_OK);
}

void test_non_finite_samples_are_contained() {
    dg_lufs* lufs = nullptr;
    dg_lufs_config config = make_config(48000, 2);
    EXPECT_STATUS(dg_lufs_create(&config, &lufs), DG_OK);
    if (lufs == nullptr) {
        return;
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> broken(24000 * 2, nan);
    EXPECT_STATUS(dg_lufs_process(lufs, view_of(broken, 2)), DG_OK);
    dg_lufs_values values = snapshot(lufs);
    CHECK(values.non_finite_samples == 48000U);
    CHECK(values.integrated_ready == 0);
    CHECK(std::isinf(values.momentary_lufs));

    /* The recursive filter state must not stay poisoned: a clean tone afterwards
     * has to read normally. */
    const std::vector<float> sine = tone(48000, 2, 48000.0, 1000.0, 0.5, 0.5);
    EXPECT_STATUS(dg_lufs_process(lufs, view_of(sine, 2)), DG_OK);
    values = snapshot(lufs);
    CHECK(values.non_finite_samples == 48000U);
    CHECK(values.integrated_ready == 1);
    CHECK(near(values.momentary_lufs, 20.0 * std::log10(0.5), 0.1));
    EXPECT_STATUS(dg_lufs_destroy(lufs), DG_OK);
}

void test_rejections_are_atomic() {
    dg_lufs* lufs = nullptr;
    dg_lufs_config config = make_config(48000, 2);
    EXPECT_STATUS(dg_lufs_create(&config, &lufs), DG_OK);
    if (lufs == nullptr) {
        return;
    }
    const std::vector<float> mono = tone(480, 1, 48000.0, 1000.0, 0.5, 0.0);
    const dg_lufs_values before = snapshot(lufs);
    EXPECT_STATUS(dg_lufs_process(lufs, view_of(mono, 1)), DG_ERR_INVALID_ARGUMENT);
    dg_pcm_view null_samples{nullptr, 480, 2};
    EXPECT_STATUS(dg_lufs_process(lufs, null_samples), DG_ERR_INVALID_ARGUMENT);
    const dg_lufs_values after = snapshot(lufs);
    CHECK(after.blocks == before.blocks);
    CHECK(after.pending_frames == before.pending_frames);
    CHECK(after.integrated_lufs == before.integrated_lufs);

    dg_pcm_view empty{nullptr, 0, 2};
    EXPECT_STATUS(dg_lufs_process(lufs, empty), DG_OK);
    CHECK(snapshot(lufs).pending_frames == before.pending_frames);
    EXPECT_STATUS(dg_lufs_destroy(lufs), DG_OK);
}

void test_reset_returns_to_fresh_state() {
    dg_lufs* lufs = nullptr;
    dg_lufs_config config = make_config(48000, 2);
    EXPECT_STATUS(dg_lufs_create(&config, &lufs), DG_OK);
    if (lufs == nullptr) {
        return;
    }
    const std::vector<float> sine = tone(48000 * 2, 2, 48000.0, 1000.0, 0.5, 0.5);
    EXPECT_STATUS(dg_lufs_process(lufs, view_of(sine, 2)), DG_OK);
    CHECK(snapshot(lufs).blocks > 10U);

    EXPECT_STATUS(dg_lufs_reset(lufs), DG_OK);
    const dg_lufs_values fresh = snapshot(lufs);
    CHECK(fresh.blocks == 0U);
    CHECK(fresh.blocks_used == 0U);
    CHECK(fresh.blocks_after_absolute_gate == 0U);
    CHECK(fresh.non_finite_samples == 0U);
    CHECK(fresh.pending_frames == 0U);
    CHECK(fresh.integrated_ready == 0);
    CHECK(fresh.hop_frames == 4800U);
    CHECK(fresh.block_frames == 19200U);
    CHECK(std::isinf(fresh.momentary_lufs));
    CHECK(std::isinf(fresh.short_term_lufs));
    CHECK(std::isinf(fresh.integrated_lufs));
    EXPECT_STATUS(dg_lufs_destroy(lufs), DG_OK);
}

void test_chunking_is_bit_identical() {
    const std::size_t frames = 48000 * 3;
    const int32_t channels = 2;
    const std::vector<float> whole = tone(frames, channels, 48000.0, 997.0, 0.5, 0.3);

    dg_lufs_config config = make_config(48000, channels);
    dg_lufs* one_shot = nullptr;
    dg_lufs* chunked = nullptr;
    EXPECT_STATUS(dg_lufs_create(&config, &one_shot), DG_OK);
    EXPECT_STATUS(dg_lufs_create(&config, &chunked), DG_OK);
    if (one_shot == nullptr || chunked == nullptr) {
        return;
    }
    EXPECT_STATUS(dg_lufs_process(one_shot, view_of(whole, channels)), DG_OK);

    /* Splits deliberately cross hop (4800) and block (19200) boundaries. */
    const std::size_t chunk_sizes[] = {1, 13, 4800, 4799, 1000, 19200, 7, 30000};
    std::size_t offset = 0;
    std::size_t chunk_index = 0;
    while (offset < frames) {
        std::size_t take = chunk_sizes[chunk_index % 8];
        if (take > frames - offset) {
            take = frames - offset;
        }
        dg_pcm_view part{};
        part.samples = whole.data() + offset * static_cast<std::size_t>(channels);
        part.frame_count = static_cast<uint32_t>(take);
        part.channel_count = channels;
        EXPECT_STATUS(dg_lufs_process(chunked, part), DG_OK);
        offset += take;
        ++chunk_index;
    }

    const dg_lufs_values a = snapshot(one_shot);
    const dg_lufs_values b = snapshot(chunked);
    CHECK(a.momentary_lufs == b.momentary_lufs);
    CHECK(a.short_term_lufs == b.short_term_lufs);
    CHECK(a.integrated_lufs == b.integrated_lufs);
    CHECK(a.blocks == b.blocks);
    CHECK(a.blocks_used == b.blocks_used);
    CHECK(a.blocks_after_absolute_gate == b.blocks_after_absolute_gate);
    CHECK(a.blocks_dropped == b.blocks_dropped);
    CHECK(a.pending_frames == b.pending_frames);
    CHECK(a.integrated_ready == b.integrated_ready);
    EXPECT_STATUS(dg_lufs_destroy(one_shot), DG_OK);
    EXPECT_STATUS(dg_lufs_destroy(chunked), DG_OK);
}

}  // namespace

int main() {
    std::printf("dg_lufs tests\n");
    run("create validation", test_create_validation);
    run("null handles", test_null_handles);
    run("k-weighting matches published table", test_k_weighting_matches_published_table);
    run("stereo calibration and channel sum", test_stereo_calibration_and_channel_sum);
    run("window availability", test_window_availability);
    run("silence is minus infinity", test_silence_is_minus_infinity);
    run("gating", test_gating);
    run("relative gate keeps near blocks", test_relative_gate_keeps_near_blocks);
    run("non-finite samples are contained", test_non_finite_samples_are_contained);
    run("rejections are atomic", test_rejections_are_atomic);
    run("reset returns to fresh state", test_reset_returns_to_fresh_state);
    run("chunking is bit-identical", test_chunking_is_bit_identical);
    std::printf("  %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
