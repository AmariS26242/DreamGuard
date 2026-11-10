// Tests for the DG-013 compressor.
//
// Run on the host through CTest, same style as the other test programs.
//
// What these tests establish:
//   * the create() validation matrix and the exact error codes, including that a
//     rejected create clears its out-parameter;
//   * descriptor validation: NULL handles, channel/frame mismatches between the
//     views, a zero-frame no-op, and the aliasing contract (exact in-place is
//     supported, partial overlap is rejected without writing);
//   * the static curve is exactly the documented piecewise function: hard knee
//     above/below the threshold, the quadratic soft knee, continuity at the knee
//     edges, and ratio 1 as a pass-through;
//   * gain reduction on a real stream converges to the curve's value for a
//     constant-magnitude signal, and a 1 kHz sine lands within 0.5 dB of it
//     (a per-sample detector plus gain smoothing cannot be exact on a sparse
//     waveform, and the test says so instead of pretending);
//   * the attack and release obey the documented one-pole law to 1e-6 dB at
//     whole time constants, and the release gives back exactly what the attack
//     took;
//   * stereo linking applies one gain to both channels (the ratio between them
//     survives) while unlinked channels are independent;
//   * reset() restores a fresh handle state bit for bit;
//   * any chunking of the same stream produces bit-identical output;
//   * the gain never jumps: with a 10 ms attack the per-sample gain change stays
//     under (1 - exp(-1 / 480)) * target, which is the click-free property this
//     design can actually promise (there is no lookahead);
//   * non-finite input samples are counted and replaced with 0, and the gain
//     state recovers;
//   * makeup gain can push the output past full scale, and those samples are
//     counted in clip_samples (this component has no limiter: that is DG-014).
//
// What these tests do NOT establish: no listening test, no reference against a
// commercial compressor, no true-peak behaviour, no program-dependent release,
// and no claim that a given attack/release value sounds like any hardware unit.
// Nothing here has been measured on a device: the numbers below are only as good
// as an actual run of this file.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "dreamguard/compressor.h"
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

void expect_close(double actual, double expected, double tolerance, const char* what,
                  int line) {
    ++g_checks;
    if (std::fabs(actual - expected) <= tolerance) {
        return;
    }
    ++g_failures;
    std::printf("    FAIL line %d: %s: %.6f, expected %.6f +/- %g\n", line, what, actual,
                expected, tolerance);
}

#define CHECK(expression) expect((expression), #expression, __LINE__)
#define EXPECT_STATUS(call, expected) \
    expect_status(#call, (call), (expected), __LINE__)
#define CHECK_CLOSE(actual, expected, tolerance) \
    expect_close((actual), (expected), (tolerance), #actual, __LINE__)

constexpr double kPi = 3.14159265358979323846;
constexpr double kNegativeInfinity = -std::numeric_limits<double>::infinity();

void run(const char* name, void (*test)()) {
    std::printf("  %s\n", name);
    test();
}

dg_compressor* invalid_handle() {
    return reinterpret_cast<dg_compressor*>(static_cast<std::uintptr_t>(1));
}

double dbfs_to_linear(double dbfs) { return std::pow(10.0, dbfs / 20.0); }

double linear_to_dbfs(double linear) {
    if (!(linear > 0.0)) {
        return kNegativeInfinity;
    }
    return 20.0 * std::log10(linear);
}

dg_compressor_config base_config(int32_t rate = 48000, int32_t channels = 2) {
    dg_compressor_config config{};
    config.sample_rate = rate;
    config.channel_count = channels;
    config.threshold_db = -20.0;
    config.ratio = 4.0;
    config.knee_db = 0.0;
    config.attack_ms = 1.0;
    config.release_ms = 50.0;
    config.link_stereo = 1;
    config.makeup_gain_db = 0.0;
    return config;
}

dg_pcm_view view_of(const std::vector<float>& samples, int32_t channels) {
    dg_pcm_view view{};
    view.samples = samples.data();
    view.frame_count =
        static_cast<uint32_t>(samples.size() / static_cast<std::size_t>(channels));
    view.channel_count = channels;
    return view;
}

dg_pcm_buffer mutable_buffer_of(std::vector<float>& samples, int32_t channels) {
    dg_pcm_buffer buffer{};
    buffer.samples = samples.data();
    buffer.frame_count =
        static_cast<uint32_t>(samples.size() / static_cast<std::size_t>(channels));
    buffer.channel_count = channels;
    return buffer;
}

/* Constant-magnitude test signal: +A, -A alternating every sample, so |x| is A
 * on every frame. The detector sees one level, the curve produces one target and
 * the ballistics become exactly predictable. A sine cannot offer that (its
 * per-sample magnitude varies), so the sine case below is a bound, not an
 * equality. */
std::vector<float> square(std::size_t frames, int32_t channels, double amplitude) {
    std::vector<float> samples(frames * static_cast<std::size_t>(channels), 0.0F);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double value = (frame % 2U == 0U) ? amplitude : -amplitude;
        for (int32_t ch = 0; ch < channels; ++ch) {
            samples[frame * static_cast<std::size_t>(channels) +
                    static_cast<std::size_t>(ch)] = static_cast<float>(value);
        }
    }
    return samples;
}

/* Constant magnitude per channel, different per channel: used for the stereo
 * link cases. */
std::vector<float> square_pair(std::size_t frames, double left_amplitude,
                               double right_amplitude) {
    std::vector<float> samples(frames * 2U, 0.0F);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        samples[frame * 2U] = static_cast<float>((frame % 2U == 0U)
                                                     ? left_amplitude
                                                     : -left_amplitude);
        samples[frame * 2U + 1] = static_cast<float>((frame % 2U == 0U)
                                                         ? right_amplitude
                                                         : -right_amplitude);
    }
    return samples;
}

std::vector<float> tone(std::size_t frames, int32_t channels, double rate,
                        double frequency, double amplitude) {
    std::vector<float> samples(frames * static_cast<std::size_t>(channels), 0.0F);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double value = amplitude * std::sin(2.0 * kPi * frequency *
                                                  static_cast<double>(frame) / rate);
        for (int32_t ch = 0; ch < channels; ++ch) {
            samples[frame * static_cast<std::size_t>(channels) +
                    static_cast<std::size_t>(ch)] = static_cast<float>(value);
        }
    }
    return samples;
}

double peak_abs(const std::vector<float>& samples) {
    double peak = 0.0;
    for (float sample : samples) {
        const double magnitude = std::fabs(static_cast<double>(sample));
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    return peak;
}

struct RunResult {
    std::vector<float> output;
    dg_compressor_values values{};
    bool ok = false;
};

/* Runs the same block twice through one handle, so the second pass shows settled
 * state without the startup transient (the gain starts at 0 dB of reduction).
 * `output` holds the second pass, i.e. the settled samples. */
struct TwoBlockResult {
    std::vector<float> output;
    dg_compressor_values before{};
    dg_compressor_values after{};
    bool ok = false;
};

TwoBlockResult run_two_blocks(const dg_compressor_config& config,
                              const std::vector<float>& block) {
    TwoBlockResult result;
    dg_compressor* compressor = nullptr;
    if (dg_compressor_create(&config, &compressor) != DG_OK) {
        return result;
    }
    result.output.assign(block.size(), 0.0F);
    bool ok = true;
    for (int pass = 0; pass < 2 && ok; ++pass) {
        ok = dg_compressor_process(compressor, view_of(block, config.channel_count),
                                   mutable_buffer_of(result.output, config.channel_count)) ==
             DG_OK;
        if (ok) {
            ok = dg_compressor_snapshot(compressor,
                                        pass == 0 ? &result.before : &result.after) ==
                 DG_OK;
        }
    }
    dg_compressor_destroy(compressor);
    result.ok = ok;
    return result;
}

RunResult run_stream(const dg_compressor_config& config,
                     const std::vector<float>& input) {
    RunResult result;
    dg_compressor* compressor = nullptr;
    if (dg_compressor_create(&config, &compressor) != DG_OK) {
        return result;
    }
    result.output.assign(input.size(), 0.0F);
    const dg_pcm_view in = view_of(input, config.channel_count);
    const dg_pcm_buffer out = mutable_buffer_of(result.output, config.channel_count);
    result.ok = dg_compressor_process(compressor, in, out) == DG_OK;
    if (result.ok) {
        result.ok = dg_compressor_snapshot(compressor, &result.values) == DG_OK;
    }
    dg_compressor_destroy(compressor);
    return result;
}

void test_create_validation() {
    dg_compressor* compressor = invalid_handle();

    EXPECT_STATUS(dg_compressor_create(nullptr, &compressor), DG_ERR_INVALID_ARGUMENT);
    CHECK(compressor == nullptr);
    compressor = invalid_handle();
    EXPECT_STATUS(dg_compressor_create(nullptr, nullptr), DG_ERR_INVALID_ARGUMENT);

    dg_compressor_config config = base_config();
    compressor = invalid_handle();
    EXPECT_STATUS(dg_compressor_create(&config, &compressor), DG_OK);
    CHECK(compressor != nullptr);
    EXPECT_STATUS(dg_compressor_destroy(compressor), DG_OK);

    compressor = invalid_handle();
    config.sample_rate = 7999;
    EXPECT_STATUS(dg_compressor_create(&config, &compressor),
                  DG_ERR_SAMPLE_RATE_OUT_OF_RANGE);
    CHECK(compressor == nullptr);
    config.sample_rate = 192001;
    EXPECT_STATUS(dg_compressor_create(&config, &compressor),
                  DG_ERR_SAMPLE_RATE_OUT_OF_RANGE);

    config = base_config();
    config.channel_count = 0;
    EXPECT_STATUS(dg_compressor_create(&config, &compressor),
                  DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE);
    config.channel_count = 3;
    EXPECT_STATUS(dg_compressor_create(&config, &compressor),
                  DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE);

    /* Parameter matrix: every documented boundary is accepted, one step past it
     * is rejected. */
    struct Field {
        double dg_compressor_config::*member;
        double minimum;
        double maximum;
    };
    const Field fields[] = {
        {&dg_compressor_config::threshold_db, DG_COMPRESSOR_THRESHOLD_DB_MIN,
         DG_COMPRESSOR_THRESHOLD_DB_MAX},
        {&dg_compressor_config::ratio, DG_COMPRESSOR_RATIO_MIN, DG_COMPRESSOR_RATIO_MAX},
        {&dg_compressor_config::knee_db, DG_COMPRESSOR_KNEE_DB_MIN,
         DG_COMPRESSOR_KNEE_DB_MAX},
        {&dg_compressor_config::attack_ms, DG_COMPRESSOR_ATTACK_MS_MIN,
         DG_COMPRESSOR_ATTACK_MS_MAX},
        {&dg_compressor_config::release_ms, DG_COMPRESSOR_RELEASE_MS_MIN,
         DG_COMPRESSOR_RELEASE_MS_MAX},
        {&dg_compressor_config::makeup_gain_db, DG_COMPRESSOR_MAKEUP_DB_MIN,
         DG_COMPRESSOR_MAKEUP_DB_MAX},
    };
    for (const Field& field : fields) {
        config = base_config();
        config.*field.member = field.minimum;
        compressor = nullptr;
        EXPECT_STATUS(dg_compressor_create(&config, &compressor), DG_OK);
        if (compressor != nullptr) {
            EXPECT_STATUS(dg_compressor_destroy(compressor), DG_OK);
        }
        config = base_config();
        config.*field.member = field.maximum;
        compressor = nullptr;
        EXPECT_STATUS(dg_compressor_create(&config, &compressor), DG_OK);
        if (compressor != nullptr) {
            EXPECT_STATUS(dg_compressor_destroy(compressor), DG_OK);
        }

        config = base_config();
        config.*field.member = field.minimum - 0.5;
        compressor = invalid_handle();
        EXPECT_STATUS(dg_compressor_create(&config, &compressor),
                      DG_ERR_INVALID_ARGUMENT);
        CHECK(compressor == nullptr);
        config = base_config();
        config.*field.member = field.maximum + 0.5;
        EXPECT_STATUS(dg_compressor_create(&config, &compressor),
                      DG_ERR_INVALID_ARGUMENT);

        config = base_config();
        config.*field.member = std::numeric_limits<double>::quiet_NaN();
        EXPECT_STATUS(dg_compressor_create(&config, &compressor),
                      DG_ERR_INVALID_ARGUMENT);
        config = base_config();
        config.*field.member = std::numeric_limits<double>::infinity();
        EXPECT_STATUS(dg_compressor_create(&config, &compressor),
                      DG_ERR_INVALID_ARGUMENT);
    }

    config = base_config();
    config.link_stereo = 2;
    EXPECT_STATUS(dg_compressor_create(&config, &compressor), DG_ERR_INVALID_ARGUMENT);
    config.link_stereo = -1;
    EXPECT_STATUS(dg_compressor_create(&config, &compressor), DG_ERR_INVALID_ARGUMENT);
    config.link_stereo = 1;
    EXPECT_STATUS(dg_compressor_create(&config, &compressor), DG_OK);
    if (compressor != nullptr) {
        EXPECT_STATUS(dg_compressor_destroy(compressor), DG_OK);
    }
}

void test_null_and_descriptor_validation() {
    dg_compressor_values values{};
    EXPECT_STATUS(dg_compressor_destroy(nullptr), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_compressor_reset(nullptr), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_compressor_snapshot(nullptr, &values), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_compressor_process(nullptr, dg_pcm_view{}, dg_pcm_buffer{}),
                  DG_ERR_NULL_HANDLE);

    dg_compressor_config config = base_config();
    dg_compressor* compressor = nullptr;
    EXPECT_STATUS(dg_compressor_create(&config, &compressor), DG_OK);
    CHECK(compressor != nullptr);
    EXPECT_STATUS(dg_compressor_snapshot(compressor, nullptr), DG_ERR_INVALID_ARGUMENT);

    std::vector<float> input = square(480, 2, 0.5);
    std::vector<float> output(input.size(), 0.0F);

    dg_pcm_view in = view_of(input, 2);
    dg_pcm_buffer out = mutable_buffer_of(output, 2);

    /* Zero frames is a no-op even with null pointers and a foreign channel
     * count, and it must not disturb the counters. */
    dg_pcm_view empty{};
    empty.frame_count = 0;
    empty.channel_count = 7;
    dg_pcm_buffer empty_out{};
    empty_out.frame_count = 0;
    empty_out.channel_count = 7;
    EXPECT_STATUS(dg_compressor_process(compressor, empty, empty_out), DG_OK);
    EXPECT_STATUS(dg_compressor_snapshot(compressor, &values), DG_OK);
    CHECK(values.frames == 0U);
    CHECK(values.gain_reduction_db == 0.0);

    /* Channel mismatch and frame-count mismatch are rejected before any write. */
    dg_pcm_view mono_in = in;
    mono_in.channel_count = 1;
    EXPECT_STATUS(dg_compressor_process(compressor, mono_in, out), DG_ERR_INVALID_ARGUMENT);
    dg_pcm_buffer short_out = out;
    short_out.frame_count = out.frame_count - 1;
    EXPECT_STATUS(dg_compressor_process(compressor, in, short_out),
                  DG_ERR_INVALID_ARGUMENT);

    /* Null samples with frames > 0. */
    dg_pcm_view null_in = in;
    null_in.samples = nullptr;
    EXPECT_STATUS(dg_compressor_process(compressor, null_in, out),
                  DG_ERR_INVALID_ARGUMENT);
    dg_pcm_buffer null_out = out;
    null_out.samples = nullptr;
    EXPECT_STATUS(dg_compressor_process(compressor, in, null_out),
                  DG_ERR_INVALID_ARGUMENT);

    /* Partial overlap is rejected and writes nothing. */
    for (float& sample : output) {
        sample = 123.0F;
    }
    dg_pcm_buffer overlapping = out;
    overlapping.samples = input.data() + 1;
    EXPECT_STATUS(dg_compressor_process(compressor, in, overlapping),
                  DG_ERR_INVALID_ARGUMENT);
    bool untouched = true;
    for (float sample : output) {
        if (sample != 123.0F) {
            untouched = false;
        }
    }
    CHECK(untouched);

    /* A rejected call leaves the counters alone. */
    EXPECT_STATUS(dg_compressor_snapshot(compressor, &values), DG_OK);
    CHECK(values.frames == 0U);
    CHECK(values.clip_samples == 0U);
    EXPECT_STATUS(dg_compressor_destroy(compressor), DG_OK);
}

void test_static_curve() {
    dg_compressor_config config = base_config();
    double gain = 1.0;

    /* Hard knee, 4:1, threshold -20 dBFS. At exactly the threshold the answer is
     * exactly 0.0 in double - no tolerance involved. */
    EXPECT_STATUS(dg_compressor_static_gain_db(&config, -30.0, &gain), DG_OK);
    CHECK_CLOSE(gain, 0.0, 1e-12);
    EXPECT_STATUS(dg_compressor_static_gain_db(&config, -20.0, &gain), DG_OK);
    CHECK(gain == 0.0);
    EXPECT_STATUS(dg_compressor_static_gain_db(&config, -10.0, &gain), DG_OK);
    CHECK_CLOSE(gain, -7.5, 1e-12);
    EXPECT_STATUS(dg_compressor_static_gain_db(&config, 0.0, &gain), DG_OK);
    CHECK_CLOSE(gain, -15.0, 1e-12);

    /* 2:1 puts +20 dB in at +10 dB out, i.e. -10 dB of reduction. */
    config.ratio = 2.0;
    EXPECT_STATUS(dg_compressor_static_gain_db(&config, 0.0, &gain), DG_OK);
    CHECK_CLOSE(gain, -10.0, 1e-12);

    /* 100:1. */
    config.ratio = 100.0;
    EXPECT_STATUS(dg_compressor_static_gain_db(&config, 0.0, &gain), DG_OK);
    CHECK_CLOSE(gain, -19.8, 1e-12);

    /* Ratio 1 is a pass-through at any level. */
    config.ratio = 1.0;
    for (double level_db : {-120.0, -30.0, -20.0, 0.0}) {
        EXPECT_STATUS(dg_compressor_static_gain_db(&config, level_db, &gain), DG_OK);
        CHECK_CLOSE(gain, 0.0, 1e-12);
    }

    /* Soft knee, 6 dB, centred on the threshold: the blend is quadratic, so the
     * midpoint is a quarter of the way, and the edges are continuous with the
     * hard-knee branches. */
    config = base_config();
    config.knee_db = 6.0;
    EXPECT_STATUS(dg_compressor_static_gain_db(&config, -23.0, &gain), DG_OK);
    CHECK_CLOSE(gain, 0.0, 1e-12);
    EXPECT_STATUS(dg_compressor_static_gain_db(&config, -20.0, &gain), DG_OK);
    CHECK_CLOSE(gain, -0.5625, 1e-12);
    EXPECT_STATUS(dg_compressor_static_gain_db(&config, -19.0, &gain), DG_OK);
    CHECK_CLOSE(gain, -1.0, 1e-12);
    EXPECT_STATUS(dg_compressor_static_gain_db(&config, -17.0, &gain), DG_OK);
    CHECK_CLOSE(gain, -2.25, 1e-12);
    EXPECT_STATUS(dg_compressor_static_gain_db(&config, -10.0, &gain), DG_OK);
    CHECK_CLOSE(gain, -7.5, 1e-12);

    /* The knee is monotone and never less reducing than the hard knee. */
    double previous = 0.0;
    bool monotone = true;
    for (double level_db = -24.0; level_db <= -16.0; level_db += 0.25) {
        EXPECT_STATUS(dg_compressor_static_gain_db(&config, level_db, &gain), DG_OK);
        if (gain > previous + 1e-12) {
            monotone = false;
        }
        previous = gain;
    }
    CHECK(monotone);

    /* 24 dB knee at the threshold: -0.75 * 144 / 48 = -2.25 dB. */
    config.knee_db = 24.0;
    EXPECT_STATUS(dg_compressor_static_gain_db(&config, -20.0, &gain), DG_OK);
    CHECK_CLOSE(gain, -2.25, 1e-12);

    /* Validation. */
    config = base_config();
    EXPECT_STATUS(dg_compressor_static_gain_db(nullptr, -10.0, &gain),
                  DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_compressor_static_gain_db(&config, -10.0, nullptr),
                  DG_ERR_INVALID_ARGUMENT);
    gain = 123.0;
    EXPECT_STATUS(dg_compressor_static_gain_db(
                      &config, std::numeric_limits<double>::quiet_NaN(), &gain),
                  DG_ERR_INVALID_ARGUMENT);
    CHECK(gain == 0.0);
    config.ratio = 0.5;
    EXPECT_STATUS(dg_compressor_static_gain_db(&config, -10.0, &gain),
                  DG_ERR_INVALID_ARGUMENT);
    config = base_config();
    config.knee_db = 30.0;
    EXPECT_STATUS(dg_compressor_static_gain_db(&config, -10.0, &gain),
                  DG_ERR_INVALID_ARGUMENT);
}

void test_pass_through() {
    dg_compressor_config config = base_config();
    config.ratio = 1.0;
    config.makeup_gain_db = 0.0;
    const std::vector<float> input = tone(4800, 2, 48000.0, 1000.0, 0.5);
    const RunResult result = run_stream(config, input);
    CHECK(result.ok);
    CHECK(result.values.gain_reduction_db == 0.0);
    CHECK(result.values.static_gain_reduction_db == 0.0);
    CHECK(result.values.frames == 4800U);

    bool identical = true;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (result.output[i] != input[i]) {
            identical = false;
        }
    }
    CHECK(identical);
    CHECK_CLOSE(result.values.input_peak_db, linear_to_dbfs(0.5), 1e-9);
    CHECK_CLOSE(result.values.output_peak_db, linear_to_dbfs(0.5), 1e-9);
}

void test_steady_state_square() {
    /* -6.0206 dBFS peak, threshold -20, 4:1, hard knee: 13.9794 dB over the
     * threshold becomes 10.4845 dB of reduction. */
    dg_compressor_config config = base_config();
    const double level_db = linear_to_dbfs(0.5);
    const double expected_reduction = (level_db - config.threshold_db) * (1.0 / 4.0 - 1.0);
    CHECK_CLOSE(expected_reduction, -10.4845, 1e-3);

    /* The snapshot peaks cover the WHOLE last block, so the first block still
     * carries the attack: the gain starts at 0 dB and only ever decreases, so
     * the loudest sample of the first block is its first sample. Run two 1 s
     * blocks through one handle and check both blocks against their own
     * analytic value instead of pretending the first one is settled. */
    const TwoBlockResult blocks = run_two_blocks(config, square(48000, 2, 0.5));
    CHECK(blocks.ok);
    const dg_compressor_values& attack = blocks.before;
    const dg_compressor_values& settled = blocks.after;

    /* 1 s at a 1 ms attack leaves the follower converged to better than 1e-9. */
    CHECK_CLOSE(settled.gain_reduction_db, expected_reduction, 1e-6);
    CHECK_CLOSE(settled.static_gain_reduction_db, expected_reduction, 1e-12);
    CHECK_CLOSE(attack.static_gain_reduction_db, expected_reduction, 1e-12);
    CHECK(settled.frames == 96000U);

    /* First block: the peak sits on the first sample, one attack step into the
     * reduction (the smoothing is applied before the sample is written). */
    const double attack_coefficient =
        1.0 - std::exp(-1.0 / (config.attack_ms * 0.001 * 48000.0));
    CHECK_CLOSE(attack.output_peak_db,
                level_db + expected_reduction * attack_coefficient, 1e-5);

    /* Second block: the meter now shows the settled peak, which is the value the
     * reduction actually delivers. A float sample costs ~5e-7 dB here. */
    CHECK_CLOSE(settled.output_peak_db, level_db + expected_reduction, 1e-5);
    CHECK_CLOSE(attack.input_peak_db, level_db, 1e-5);
    CHECK_CLOSE(settled.input_peak_db, level_db, 1e-5);

    const double expected_output = dbfs_to_linear(level_db + expected_reduction);
    CHECK_CLOSE(std::fabs(static_cast<double>(blocks.output.back())), expected_output,
                1e-6);
    CHECK(settled.clip_samples == 0U);
    CHECK(settled.non_finite_samples == 0U);
    CHECK(settled.linked == 1);
}

void test_soft_knee_on_stream() {
    /* Level 6 dB over the threshold with a 6 dB knee sits exactly at the upper
     * knee edge, where the curve must equal the hard-knee value. */
    dg_compressor_config config = base_config();
    config.knee_db = 6.0;
    const std::vector<float> input = square(48000, 2, dbfs_to_linear(-14.0));
    const RunResult result = run_stream(config, input);
    CHECK(result.ok);
    CHECK_CLOSE(result.values.gain_reduction_db, -4.5, 1e-6);

    /* "Exactly at the threshold" cannot be built from a float sample: the closest
     * float to 10^(-20/20) is 0.100000001490116, i.e. 1.3e-7 dB ABOVE -20 dBFS,
     * so the curve must produce a tiny, well-defined reduction here - not zero.
     * Both the level and the expectation below are computed from the sample the
     * compressor actually sees, so the check is strict instead of loose. */
    const float near_amplitude = static_cast<float>(dbfs_to_linear(-20.0));
    const double near_level_db = linear_to_dbfs(static_cast<double>(near_amplitude));
    CHECK(near_level_db > config.threshold_db);
    CHECK(near_level_db < config.threshold_db + 1e-6);
    const std::vector<float> at_threshold = square(48000, 2, near_amplitude);

    /* Soft knee at the threshold: the quadratic blend is flat there, so the
     * reduction is -0.5625 dB to within 1e-7 dB. (knee_db is still 6.0 here.) */
    CHECK(config.knee_db == 6.0);
    const RunResult threshold_result = run_stream(config, at_threshold);
    CHECK(threshold_result.ok);
    CHECK_CLOSE(threshold_result.values.static_gain_reduction_db, -0.5625, 1e-7);
    CHECK_CLOSE(threshold_result.values.gain_reduction_db, -0.5625, 1e-6);

    /* Hard knee at the same level: exactly slope * x for the x the sample really
     * has. This is the boundary case the run exposed (it used to expect a hard
     * 0.0 that a float sample cannot represent). */
    config.knee_db = 0.0;
    const RunResult hard_result = run_stream(config, at_threshold);
    CHECK(hard_result.ok);
    const double expected_hard =
        (near_level_db - config.threshold_db) * (1.0 / 4.0 - 1.0);
    CHECK(expected_hard < 0.0);
    CHECK(expected_hard > -1e-6);
    CHECK_CLOSE(hard_result.values.static_gain_reduction_db, expected_hard, 1e-12);
    CHECK_CLOSE(hard_result.values.gain_reduction_db, expected_hard, 1e-9);

    /* A level safely below the threshold must reduce by exactly nothing: the
     * target is 0.0 and the state starts at 0.0, so the equality is exact. */
    const RunResult below = run_stream(config, square(48000, 2, dbfs_to_linear(-20.5)));
    CHECK(below.ok);
    CHECK(below.values.static_gain_reduction_db == 0.0);
    CHECK(below.values.gain_reduction_db == 0.0);
}

void test_attack_and_release_law() {
    /* Constant magnitude, so the target is a step: -60 dBFS then -10 dBFS. */
    dg_compressor_config config = base_config();
    config.attack_ms = 10.0;
    config.release_ms = 10.0;

    dg_compressor* compressor = nullptr;
    EXPECT_STATUS(dg_compressor_create(&config, &compressor), DG_OK);
    if (compressor == nullptr) {
        CHECK(false);
        return;
    }

    std::vector<float> input = square(480 * 10, 2, 0.001);
    std::vector<float> output(input.size(), 0.0F);
    EXPECT_STATUS(dg_compressor_process(compressor, view_of(input, 2),
                                        mutable_buffer_of(output, 2)), DG_OK);

    dg_compressor_values values{};
    EXPECT_STATUS(dg_compressor_snapshot(compressor, &values), DG_OK);
    CHECK_CLOSE(values.gain_reduction_db, 0.0, 1e-12);

    const double target = (-10.0 - config.threshold_db) * (1.0 / 4.0 - 1.0);
    CHECK_CLOSE(target, -7.5, 1e-12);

    /* One attack time constant: 1 - exp(-1) of the way. */
    std::vector<float> step = square(480, 2, dbfs_to_linear(-10.0));
    std::vector<float> step_output(step.size(), 0.0F);
    EXPECT_STATUS(dg_compressor_process(compressor, view_of(step, 2),
                                        mutable_buffer_of(step_output, 2)), DG_OK);
    EXPECT_STATUS(dg_compressor_snapshot(compressor, &values), DG_OK);
    CHECK_CLOSE(values.gain_reduction_db, target * (1.0 - std::exp(-1.0)), 1e-6);

    /* Five more time constants: 1 - exp(-6). */
    std::vector<float> more = square(480 * 5, 2, dbfs_to_linear(-10.0));
    std::vector<float> more_output(more.size(), 0.0F);
    EXPECT_STATUS(dg_compressor_process(compressor, view_of(more, 2),
                                        mutable_buffer_of(more_output, 2)), DG_OK);
    EXPECT_STATUS(dg_compressor_snapshot(compressor, &values), DG_OK);
    CHECK_CLOSE(values.gain_reduction_db, target * (1.0 - std::exp(-6.0)), 1e-6);

    /* Settle completely, then measure the release: one time constant gives back
     * 1 - exp(-1) of the reduction. */
    std::vector<float> settle = square(480 * 20, 2, dbfs_to_linear(-10.0));
    std::vector<float> settle_output(settle.size(), 0.0F);
    EXPECT_STATUS(dg_compressor_process(compressor, view_of(settle, 2),
                                        mutable_buffer_of(settle_output, 2)), DG_OK);
    EXPECT_STATUS(dg_compressor_snapshot(compressor, &values), DG_OK);
    const double settled = values.gain_reduction_db;
    CHECK_CLOSE(settled, target, 1e-6);

    std::vector<float> quiet = square(480, 2, 0.001);
    std::vector<float> quiet_output(quiet.size(), 0.0F);
    EXPECT_STATUS(dg_compressor_process(compressor, view_of(quiet, 2),
                                        mutable_buffer_of(quiet_output, 2)), DG_OK);
    EXPECT_STATUS(dg_compressor_snapshot(compressor, &values), DG_OK);
    CHECK_CLOSE(values.gain_reduction_db, settled * std::exp(-1.0), 1e-6);

    /* The law is in frames, so the same time constant at 8 kHz must take the
     * same number of 8 kHz samples. */
    dg_compressor_config slow = base_config(8000, 1);
    slow.attack_ms = 10.0;
    dg_compressor* slow_compressor = nullptr;
    EXPECT_STATUS(dg_compressor_create(&slow, &slow_compressor), DG_OK);
    if (slow_compressor != nullptr) {
        std::vector<float> run_quiet = square(800, 1, 0.001);
        std::vector<float> scratch(run_quiet.size(), 0.0F);
        EXPECT_STATUS(dg_compressor_process(slow_compressor, view_of(run_quiet, 1),
                                            mutable_buffer_of(scratch, 1)), DG_OK);
        std::vector<float> run_loud = square(80, 1, dbfs_to_linear(-10.0));
        std::vector<float> loud_out(run_loud.size(), 0.0F);
        EXPECT_STATUS(dg_compressor_process(slow_compressor, view_of(run_loud, 1),
                                            mutable_buffer_of(loud_out, 1)), DG_OK);
        dg_compressor_values slow_values{};
        EXPECT_STATUS(dg_compressor_snapshot(slow_compressor, &slow_values), DG_OK);
        CHECK_CLOSE(slow_values.gain_reduction_db, target * (1.0 - std::exp(-1.0)), 1e-6);
        EXPECT_STATUS(dg_compressor_destroy(slow_compressor), DG_OK);
    }
    EXPECT_STATUS(dg_compressor_destroy(compressor), DG_OK);
}

void test_sine_bound() {
    /* A 1 kHz sine at 48 kHz has a magnitude that changes every sample, so the
     * per-sample detector plus gain smoothing settles near, but not exactly on,
     * the peak-based curve value. The honest assertion is a bound: within 0.5 dB
     * of the peak value and always at least as much reduction as 6 dB (the sine's
     * RMS-based estimate would be the lower bound). */
    dg_compressor_config config = base_config();
    const std::vector<float> input = tone(48000, 2, 48000.0, 1000.0, 0.5);
    const RunResult result = run_stream(config, input);
    CHECK(result.ok);

    const double peak_based = (linear_to_dbfs(0.5) - config.threshold_db) * (1.0 / 4.0 - 1.0);
    CHECK(result.values.gain_reduction_db <= 0.0);
    CHECK(result.values.gain_reduction_db >= peak_based - 0.5);
    CHECK(result.values.gain_reduction_db <= -6.0);

    /* The output must stay inside the input's envelope (the compressor only ever
     * reduces, and the makeup gain is 0). */
    CHECK(peak_abs(result.output) <= peak_abs(input) + 1e-6);
    CHECK(result.values.clip_samples == 0U);
}

void test_stereo_link() {
    const double loud = dbfs_to_linear(-6.0);
    const double quiet = dbfs_to_linear(-60.0);
    const std::vector<float> input = square_pair(48000, loud, quiet);

    dg_compressor_config linked = base_config();
    linked.link_stereo = 1;
    const RunResult linked_result = run_stream(linked, input);
    CHECK(linked_result.ok);
    CHECK(linked_result.values.linked == 1);

    const double expected = (linear_to_dbfs(loud) - linked.threshold_db) * (1.0 / 4.0 - 1.0);
    CHECK_CLOSE(linked_result.values.gain_reduction_db, expected, 1e-6);

    /* One gain for both channels: the ratio between them survives exactly. Both
     * samples are floats, so the ratio is compared to 1e-6 relative, not 1e-6
     * absolute. */
    const double left = std::fabs(static_cast<double>(linked_result.output[input.size() - 2]));
    const double right = std::fabs(static_cast<double>(linked_result.output[input.size() - 1]));
    CHECK_CLOSE(left / right, loud / quiet, (loud / quiet) * 1e-6);
    CHECK(right > quiet / 4.0); /* the quiet channel was compressed too */

    dg_compressor_config independent = base_config();
    independent.link_stereo = 0;
    const RunResult independent_result = run_stream(independent, input);
    CHECK(independent_result.ok);
    CHECK(independent_result.values.linked == 0);
    const double independent_left =
        std::fabs(static_cast<double>(independent_result.output[input.size() - 2]));
    const double independent_right =
        std::fabs(static_cast<double>(independent_result.output[input.size() - 1]));
    CHECK_CLOSE(independent_right, quiet, 1e-9);
    CHECK_CLOSE(independent_left, dbfs_to_linear(-6.0 + expected), 1e-6);

    /* Mono handles must be accepted as well. */
    dg_compressor_config mono_config = base_config(48000, 1);
    const std::vector<float> mono_input = square(4800, 1, 0.5);
    const RunResult mono_result = run_stream(mono_config, mono_input);
    CHECK(mono_result.ok);
    const double mono_expected =
        (linear_to_dbfs(0.5) - mono_config.threshold_db) * (1.0 / 4.0 - 1.0);
    CHECK_CLOSE(mono_result.values.gain_reduction_db, mono_expected, 1e-6);
}

void test_reset() {
    dg_compressor_config config = base_config();
    dg_compressor* compressor = nullptr;
    EXPECT_STATUS(dg_compressor_create(&config, &compressor), DG_OK);
    if (compressor == nullptr) {
        CHECK(false);
        return;
    }
    const std::vector<float> input = tone(4800, 2, 48000.0, 1000.0, 0.5);
    std::vector<float> first(input.size(), 0.0F);
    EXPECT_STATUS(dg_compressor_process(compressor, view_of(input, 2),
                                        mutable_buffer_of(first, 2)), DG_OK);
    dg_compressor_values values{};
    EXPECT_STATUS(dg_compressor_snapshot(compressor, &values), DG_OK);
    CHECK(values.gain_reduction_db < -1.0);
    CHECK(values.frames == 4800U);

    EXPECT_STATUS(dg_compressor_reset(compressor), DG_OK);
    EXPECT_STATUS(dg_compressor_snapshot(compressor, &values), DG_OK);
    CHECK(values.gain_reduction_db == 0.0);
    CHECK(values.static_gain_reduction_db == 0.0);
    CHECK(values.frames == 0U);
    CHECK(values.clip_samples == 0U);
    CHECK_CLOSE(values.input_peak_db, linear_to_dbfs(DG_COMPRESSOR_LEVEL_FLOOR), 1e-9);
    CHECK_CLOSE(values.output_peak_db, linear_to_dbfs(DG_COMPRESSOR_LEVEL_FLOOR), 1e-9);

    std::vector<float> second(input.size(), 0.0F);
    EXPECT_STATUS(dg_compressor_process(compressor, view_of(input, 2),
                                        mutable_buffer_of(second, 2)), DG_OK);
    bool identical = true;
    for (std::size_t i = 0; i < first.size(); ++i) {
        if (first[i] != second[i]) {
            identical = false;
        }
    }
    CHECK(identical);
    EXPECT_STATUS(dg_compressor_destroy(compressor), DG_OK);
}

void test_chunking_invariance() {
    const dg_compressor_config config = base_config();
    const std::vector<float> input = tone(4800, 2, 48000.0, 440.0, 0.8);

    const RunResult whole = run_stream(config, input);
    CHECK(whole.ok);

    dg_compressor* compressor = nullptr;
    EXPECT_STATUS(dg_compressor_create(&config, &compressor), DG_OK);
    if (compressor == nullptr) {
        CHECK(false);
        return;
    }
    std::vector<float> chunked(input.size(), 0.0F);
    const uint32_t chunk_sizes[] = {1U, 7U, 997U, 3U, 480U, 13U};
    std::size_t offset = 0;
    std::size_t index = 0;
    bool ok = true;
    const std::size_t frames = input.size() / 2U;
    while (offset < frames && ok) {
        std::size_t take = chunk_sizes[index % 6U];
        if (take > frames - offset) {
            take = frames - offset;
        }
        const dg_pcm_view in = view_of(input, 2);
        dg_pcm_view part_in = in;
        part_in.samples = input.data() + offset * 2U;
        part_in.frame_count = static_cast<uint32_t>(take);
        dg_pcm_buffer part_out = mutable_buffer_of(chunked, 2);
        part_out.samples = chunked.data() + offset * 2U;
        part_out.frame_count = static_cast<uint32_t>(take);
        ok = dg_compressor_process(compressor, part_in, part_out) == DG_OK;
        offset += take;
        ++index;
    }
    CHECK(ok);
    dg_compressor_values chunk_values{};
    ok = dg_compressor_snapshot(compressor, &chunk_values) == DG_OK;
    CHECK(ok);

    bool identical = true;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (chunked[i] != whole.output[i]) {
            identical = false;
        }
    }
    CHECK(identical);
    CHECK(chunk_values.frames == whole.values.frames);
    CHECK(chunk_values.clip_samples == whole.values.clip_samples);
    CHECK(chunk_values.non_finite_samples == whole.values.non_finite_samples);
    CHECK_CLOSE(chunk_values.gain_reduction_db, whole.values.gain_reduction_db, 1e-12);
    CHECK_CLOSE(chunk_values.static_gain_reduction_db,
                whole.values.static_gain_reduction_db, 1e-12);
    EXPECT_STATUS(dg_compressor_destroy(compressor), DG_OK);
}

void test_no_click_gain_steps() {
    /* Step from -60 dBFS to 0 dBFS with a 10 ms attack. The gain can move at most
     * (target - current) * (1 - exp(-1/480)) per sample, so no click is
     * possible; assert both the bound and that the gain never overshoots past the
     * target (no oscillation). */
    dg_compressor_config config = base_config();
    config.attack_ms = 10.0;
    config.release_ms = 500.0;
    const double tau_frames = 480.0;
    const double coefficient = 1.0 - std::exp(-1.0 / tau_frames);

    std::vector<float> input = square(480, 2, 0.001);
    const std::vector<float> step = square(4800, 2, 1.0);
    input.insert(input.end(), step.begin(), step.end());
    const RunResult result = run_stream(config, input);
    CHECK(result.ok);

    const double target = (0.0 - config.threshold_db) * (1.0 / 4.0 - 1.0);
    CHECK_CLOSE(target, -15.0, 1e-12);

    double previous_gain_db = 0.0;
    double largest_step = 0.0;
    bool monotone = true;
    double smallest_gain_db = 0.0;
    for (std::size_t frame = 240U; frame < input.size() / 2U; ++frame) {
        const double x = static_cast<double>(input[frame * 2U]);
        const double y = static_cast<double>(result.output[frame * 2U]);
        if (x == 0.0) {
            continue;
        }
        const double gain_db = linear_to_dbfs(std::fabs(y / x));
        const double delta = gain_db - previous_gain_db;
        if (frame > 240U && delta > 1e-12) {
            monotone = false;
        }
        if (std::fabs(delta) > largest_step) {
            largest_step = std::fabs(delta);
        }
        if (gain_db < smallest_gain_db) {
            smallest_gain_db = gain_db;
        }
        previous_gain_db = gain_db;
    }
    CHECK(monotone);
    /* A float round trip through the output buffer costs ~1e-7 dB per sample, so
     * the bound gets a 1e-6 dB allowance, not more. */
    CHECK(largest_step <= 15.0 * coefficient + 1e-6);
    CHECK(smallest_gain_db >= -15.0 - 1e-6);

    /* And the click-free property at the output: consecutive samples of a
     * constant-magnitude square differ only by the gain change. Start after the
     * quiet->loud step, which is a real signal step, not a gain step. */
    double largest_output_jump = 0.0;
    for (std::size_t i = (480U + 2U) * 2U; i < result.output.size(); i += 2U) {
        const double jump = std::fabs(std::fabs(static_cast<double>(result.output[i])) -
                                      std::fabs(static_cast<double>(result.output[i - 2U])));
        if (jump > largest_output_jump) {
            largest_output_jump = jump;
        }
    }
    /* 1.0 amplitude * (1 - 10^(-0.0312/20)) is about 0.0036 per sample. */
    CHECK(largest_output_jump < 0.005);
}

void test_makeup_gain_and_clipping() {
    dg_compressor_config config = base_config();
    config.makeup_gain_db = 24.0;
    const std::vector<float> block = square(4800, 2, 1.0);
    const TwoBlockResult result = run_two_blocks(config, block);
    CHECK(result.ok);

    /* Hard knee, 4:1, 20 dB over the threshold: -15 dB. The first block carries
     * the startup transient (the gain starts at the makeup gain), so the second
     * block is the one that shows the settled 9 dBFS peak. */
    CHECK_CLOSE(result.after.gain_reduction_db, -15.0, 1e-6);
    CHECK_CLOSE(result.after.output_peak_db, 9.0, 1e-5);
    CHECK(result.after.frames == 9600U);

    /* Every sample of the second block is over full scale: 4800 frames * 2. */
    CHECK(result.after.clip_samples - result.before.clip_samples == 9600U);
    CHECK(std::isfinite(result.after.output_peak_db));
    CHECK(std::isfinite(result.after.gain_reduction_db));

    /* Without makeup the same input is comfortably inside full scale. */
    config.makeup_gain_db = 0.0;
    const TwoBlockResult clean = run_two_blocks(config, block);
    CHECK(clean.ok);
    CHECK(clean.after.clip_samples == clean.before.clip_samples);
    CHECK_CLOSE(clean.after.output_peak_db, -15.0, 1e-5);
}

void test_non_finite_input() {
    dg_compressor_config config = base_config();
    std::vector<float> input = square(4800, 2, 0.5);
    const std::size_t nan_index = 100U;
    const std::size_t inf_index = 200U;
    const std::size_t negative_inf_index = 300U;
    input[nan_index] = std::numeric_limits<float>::quiet_NaN();
    input[inf_index] = std::numeric_limits<float>::infinity();
    input[negative_inf_index] = -std::numeric_limits<float>::infinity();

    const RunResult result = run_stream(config, input);
    CHECK(result.ok);
    CHECK(result.values.non_finite_samples == 3U);
    CHECK(result.output[nan_index] == 0.0F);
    CHECK(result.output[inf_index] == 0.0F);
    CHECK(result.output[negative_inf_index] == 0.0F);

    /* The rest of the block is still compressed, and the state recovers. */
    const double expected = (linear_to_dbfs(0.5) - config.threshold_db) * (1.0 / 4.0 - 1.0);
    CHECK_CLOSE(result.values.gain_reduction_db, expected, 1e-6);

    /* A stream that is entirely NaN produces silence and a full counter. */
    std::vector<float> all_nan(480, std::numeric_limits<float>::quiet_NaN());
    const RunResult silent = run_stream(config, all_nan);
    CHECK(silent.ok);
    CHECK(silent.values.non_finite_samples == 480U);
    CHECK(silent.values.gain_reduction_db == 0.0);
    bool all_zero = true;
    for (float sample : silent.output) {
        if (sample != 0.0F) {
            all_zero = false;
        }
    }
    CHECK(all_zero);
}

void test_aliasing() {
    dg_compressor_config config = base_config();
    const std::vector<float> input = tone(4800, 2, 48000.0, 1000.0, 0.5);

    const RunResult separate = run_stream(config, input);
    CHECK(separate.ok);

    dg_compressor* compressor = nullptr;
    EXPECT_STATUS(dg_compressor_create(&config, &compressor), DG_OK);
    if (compressor == nullptr) {
        CHECK(false);
        return;
    }
    std::vector<float> in_place = input;
    EXPECT_STATUS(dg_compressor_process(compressor, view_of(in_place, 2),
                                        mutable_buffer_of(in_place, 2)), DG_OK);
    bool identical = true;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (in_place[i] != separate.output[i]) {
            identical = false;
        }
    }
    CHECK(identical);
    EXPECT_STATUS(dg_compressor_destroy(compressor), DG_OK);
}

void test_rate_and_channel_smoke() {
    const int32_t rates[] = {8000, 44100, 48000, 96000, 192000};
    for (int32_t rate : rates) {
        for (int32_t channels = 1; channels <= 2; ++channels) {
            dg_compressor_config config = base_config(rate, channels);
            config.attack_ms = 0.1;
            config.release_ms = 1.0;
            const std::size_t frames = static_cast<std::size_t>(rate) / 10U;
            const std::vector<float> input = square(frames, channels, 0.9);
            const RunResult result = run_stream(config, input);
            CHECK(result.ok);
            CHECK(result.values.frames == static_cast<uint64_t>(frames));
            CHECK(result.values.gain_reduction_db < 0.0);
            CHECK(result.values.gain_reduction_db > -60.0);
            bool finite = std::isfinite(result.values.gain_reduction_db) &&
                          std::isfinite(result.values.input_peak_db) &&
                          std::isfinite(result.values.output_peak_db);
            CHECK(finite);
        }
    }
}

}  // namespace

int main() {
    std::printf("dg_compressor tests (DG-013)\n");

    run("create validation", test_create_validation);
    run("null handles and descriptor validation", test_null_and_descriptor_validation);
    run("static curve", test_static_curve);
    run("ratio 1 pass-through", test_pass_through);
    run("steady-state gain reduction", test_steady_state_square);
    run("soft knee on a stream", test_soft_knee_on_stream);
    run("attack and release law", test_attack_and_release_law);
    run("sine bound", test_sine_bound);
    run("stereo link", test_stereo_link);
    run("reset", test_reset);
    run("chunking invariance", test_chunking_invariance);
    run("no-click gain steps", test_no_click_gain_steps);
    run("makeup gain and clipping", test_makeup_gain_and_clipping);
    run("non-finite input", test_non_finite_input);
    run("in-place aliasing", test_aliasing);
    run("rate and channel smoke", test_rate_and_channel_smoke);

    std::printf("  %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
