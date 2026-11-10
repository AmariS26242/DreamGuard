// Tests for the portable Float32 PCM helpers (DG-011).
//
// Run on the host through CTest, same style as test_dg_lifecycle.cpp: no
// framework, one process, a non-zero exit code on failure.
//
// What these tests establish:
//   * bypass is a true bit-for-bit pass-through, including NaN payloads,
//     infinities, denormals and signed zero, in mono and in stereo;
//   * mono <-> stereo channel mapping is exact where it must be (duplicate up,
//     single-rounding down);
//   * gain is applied exactly where the arithmetic allows it, silence stays
//     silent, and out-of-contract gains are rejected rather than clamped;
//   * every entry point validates descriptors: NULL samples with a non-zero
//     frame count, unsupported channel counts, mismatched frame counts;
//   * aliasing is explicit: identical pointers are true in-place operations for
//     bypass, gain and same-layout mapping, while a channel conversion or any
//     partially overlapping buffer is rejected without writing a sample;
//   * zero-frame blocks are legal and touch nothing.
//
// What these tests do NOT establish: nothing here is an audio capture test (no
// device, no AudioRecord), nothing is wired into the playback path yet, and this
// is not a leak or race detector. docs/STATUS.md keeps DG-011 short of VERIFIED
// for exactly those reasons.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

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

// Deterministic sample material: a mix of ordinary audio values plus the bit
// patterns a naive "copy through a float DSP" would destroy.
std::vector<float> make_samples(std::size_t count, std::uint32_t seed) {
    std::vector<float> samples(count, 0.0F);
    std::uint32_t state = seed;
    const float reserved[] = {
        0.0F,
        -0.0F,
        1.0F,
        -1.0F,
        0.5F,
        -0.25F,
        1.0e-30F,                       // tiny but normal
        std::numeric_limits<float>::denorm_min(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };
    for (std::size_t i = 0; i < count; ++i) {
        if (i < sizeof(reserved) / sizeof(reserved[0])) {
            samples[i] = reserved[i];
            continue;
        }
        state = state * 1664525U + 1013904223U;
        // 24 random significand bits plus a modest exponent: ordinary audio.
        const std::uint32_t mantissa = state >> 8;
        const float unit = static_cast<float>(mantissa) / 16777216.0F;
        samples[i] = (unit * 2.0F) - 1.0F;
    }
    return samples;
}

bool same_bits(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    return a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

dg_pcm_view view_of(const std::vector<float>& samples, std::uint32_t frames,
                    std::int32_t channels) {
    return dg_pcm_view{samples.empty() ? nullptr : samples.data(), frames, channels};
}

dg_pcm_buffer buffer_of(std::vector<float>& samples, std::uint32_t frames,
                        std::int32_t channels) {
    return dg_pcm_buffer{samples.empty() ? nullptr : samples.data(), frames, channels};
}

// ---------------------------------------------------------------------------
// dg_pcm_bypass
// ---------------------------------------------------------------------------

void bypass_preserves_every_bit_mono() {
    constexpr std::uint32_t kFrames = 257;  // deliberately not a power of two
    const std::vector<float> input = make_samples(kFrames, 0x1234ABCDU);
    std::vector<float> output(kFrames, 123.0F);

    EXPECT_STATUS(dg_pcm_bypass(view_of(input, kFrames, 1), buffer_of(output, kFrames, 1)),
                  DG_OK);
    CHECK(same_bits(input, output));
    CHECK(std::memcmp(input.data(), output.data(), kFrames * sizeof(float)) == 0);
}

void bypass_preserves_every_bit_stereo() {
    constexpr std::uint32_t kFrames = 512;
    const std::size_t samples = static_cast<std::size_t>(kFrames) * 2;
    const std::vector<float> input = make_samples(samples, 0x0BADF00DU);
    std::vector<float> output(samples, -7.0F);

    EXPECT_STATUS(dg_pcm_bypass(view_of(input, kFrames, 2), buffer_of(output, kFrames, 2)),
                  DG_OK);
    CHECK(same_bits(input, output));
}

void bypass_keeps_nan_bit_patterns() {
    // A payload-carrying NaN survives only with a real memcpy; any arithmetic
    // path may canonicalize it. That is the difference bypass must preserve.
    float input[4];
    const std::uint32_t nan_bits = 0x7FC12345U;
    const std::uint32_t snan_bits = 0xFF8ABCDEU;
    std::memcpy(&input[0], &nan_bits, sizeof(float));
    std::memcpy(&input[1], &snan_bits, sizeof(float));
    input[2] = 0.0F;
    input[3] = -0.0F;

    float output[4] = {1.0F, 2.0F, 3.0F, 4.0F};
    EXPECT_STATUS(dg_pcm_bypass(dg_pcm_view{input, 4, 1}, dg_pcm_buffer{output, 4, 1}), DG_OK);
    CHECK(std::memcmp(input, output, sizeof(input)) == 0);
    CHECK(std::signbit(output[3]));
}

void bypass_accepts_an_empty_block() {
    // Zero frames with NULL samples is legal and must not touch memory.
    EXPECT_STATUS(dg_pcm_bypass(dg_pcm_view{nullptr, 0, 1}, dg_pcm_buffer{nullptr, 0, 1}),
                  DG_OK);
    EXPECT_STATUS(dg_pcm_bypass(dg_pcm_view{nullptr, 0, 2}, dg_pcm_buffer{nullptr, 0, 2}),
                  DG_OK);
}

void bypass_rejects_bad_descriptors() {
    float input[8] = {};
    float output[8] = {};

    // NULL samples with a non-zero frame count.
    EXPECT_STATUS(dg_pcm_bypass(dg_pcm_view{nullptr, 4, 1}, dg_pcm_buffer{output, 4, 1}),
                  DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_pcm_bypass(dg_pcm_view{input, 4, 1}, dg_pcm_buffer{nullptr, 4, 1}),
                  DG_ERR_INVALID_ARGUMENT);
    // Unsupported channel counts.
    EXPECT_STATUS(dg_pcm_bypass(dg_pcm_view{input, 4, 3}, dg_pcm_buffer{output, 4, 3}),
                  DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_pcm_bypass(dg_pcm_view{input, 4, 0}, dg_pcm_buffer{output, 4, 0}),
                  DG_ERR_INVALID_ARGUMENT);
    // Shape mismatch.
    EXPECT_STATUS(dg_pcm_bypass(dg_pcm_view{input, 4, 1}, dg_pcm_buffer{output, 8, 1}),
                  DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_pcm_bypass(dg_pcm_view{input, 4, 1}, dg_pcm_buffer{output, 4, 2}),
                  DG_ERR_INVALID_ARGUMENT);
}

void bypass_accepts_an_identical_buffer() {
    // The same pointer for input and output is a defined no-op, not a memcpy
    // with equal pointers (which the C standard leaves undefined).
    constexpr std::uint32_t kFrames = 64;
    std::vector<float> samples = make_samples(kFrames, 0xA11A5U);
    const std::vector<float> before = samples;
    EXPECT_STATUS(dg_pcm_bypass(view_of(samples, kFrames, 1), buffer_of(samples, kFrames, 1)),
                  DG_OK);
    CHECK(same_bits(before, samples));

    std::vector<float> stereo = make_samples(static_cast<std::size_t>(kFrames) * 2, 0xB0BU);
    const std::vector<float> stereo_before = stereo;
    EXPECT_STATUS(dg_pcm_bypass(view_of(stereo, kFrames, 2), buffer_of(stereo, kFrames, 2)),
                  DG_OK);
    CHECK(same_bits(stereo_before, stereo));
}

void bypass_rejects_partial_overlap() {
    // Distinct pointers whose ranges intersect must be rejected, not memcpy'd.
    std::vector<float> storage = make_samples(8, 0xC0FFEEU);
    const std::vector<float> before = storage;
    float* base = storage.data();
    EXPECT_STATUS(dg_pcm_bypass(dg_pcm_view{base, 4, 1}, dg_pcm_buffer{base + 1, 4, 1}),
                  DG_ERR_INVALID_ARGUMENT);
    CHECK(same_bits(before, storage));
}

// ---------------------------------------------------------------------------
// dg_pcm_apply_gain
// ---------------------------------------------------------------------------

void gain_one_is_bit_exact_for_finite_samples() {
    constexpr std::uint32_t kFrames = 333;
    const std::vector<float> input = make_samples(kFrames, 0x5EEDU);
    std::vector<float> bypassed(kFrames, 0.0F);
    std::vector<float> unity(kFrames, 0.0F);

    EXPECT_STATUS(dg_pcm_bypass(view_of(input, kFrames, 1), buffer_of(bypassed, kFrames, 1)),
                  DG_OK);
    EXPECT_STATUS(dg_pcm_apply_gain(view_of(input, kFrames, 1), buffer_of(unity, kFrames, 1),
                                    1.0F),
                  DG_OK);
    // Multiplication by 1.0F is exact for every value the generator emits
    // (finite, ±Infinity, denormal, ±0), so the two paths must agree bitwise.
    CHECK(same_bits(bypassed, unity));
}

void gain_scales_samples() {
    const std::vector<float> input = {0.5F, -0.5F, 1.0F, -1.0F, 0.0F, -0.0F, 1.0e-30F};
    std::vector<float> half(input.size(), 0.0F);
    std::vector<float> silent(input.size(), 1.0F);

    EXPECT_STATUS(dg_pcm_apply_gain(view_of(input, 7, 1), buffer_of(half, 7, 1), 0.5F), DG_OK);
    CHECK(half[0] == 0.25F);
    CHECK(half[1] == -0.25F);
    CHECK(half[2] == 0.5F);
    CHECK(half[3] == -0.5F);
    CHECK(half[4] == 0.0F);
    CHECK(std::signbit(half[5]));  // -0.0 * 0.5 keeps the sign
    CHECK(half[6] == 5.0e-31F);

    EXPECT_STATUS(dg_pcm_apply_gain(view_of(input, 7, 1), buffer_of(silent, 7, 1), 0.0F),
                  DG_OK);
    for (const float sample : silent) {
        CHECK(sample == 0.0F);
    }
}

void gain_rejects_out_of_contract_values() {
    const std::vector<float> input = {0.25F, 0.5F};
    std::vector<float> output(input.size(), 0.0F);
    const dg_pcm_view in = view_of(input, 2, 1);
    const dg_pcm_buffer out = buffer_of(output, 2, 1);

    EXPECT_STATUS(dg_pcm_apply_gain(in, out, 1.5F), DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_pcm_apply_gain(in, out, -0.5F), DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_pcm_apply_gain(in, out, std::nanf("")), DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_pcm_apply_gain(in, out, INFINITY), DG_ERR_INVALID_ARGUMENT);
    // A rejected call must not have written through the output buffer.
    CHECK(output[0] == 0.0F && output[1] == 0.0F);
}

void gain_accepts_an_identical_buffer() {
    // One read and one write per sample makes in-place gain well defined.
    std::vector<float> samples = {0.5F, -1.0F, 0.25F, -0.0F};
    const std::vector<float> before = samples;
    EXPECT_STATUS(dg_pcm_apply_gain(view_of(samples, 4, 1), buffer_of(samples, 4, 1), 0.5F),
                  DG_OK);
    CHECK(samples[0] == 0.25F);
    CHECK(samples[1] == -0.5F);
    CHECK(samples[2] == 0.125F);
    CHECK(samples[3] == 0.0F);
    CHECK(std::signbit(samples[3]));
    CHECK(before[0] == 0.5F);  // the pre-call snapshot is untouched
}

void gain_rejects_partial_overlap() {
    std::vector<float> storage = {0.5F, 0.25F, 0.125F, 0.0625F, 1.0F, 1.0F};
    const std::vector<float> before = storage;
    float* base = storage.data();
    EXPECT_STATUS(dg_pcm_apply_gain(dg_pcm_view{base, 4, 1}, dg_pcm_buffer{base + 2, 4, 1}, 0.5F),
                  DG_ERR_INVALID_ARGUMENT);
    CHECK(same_bits(before, storage));
}

// ---------------------------------------------------------------------------
// dg_pcm_map_channels
// ---------------------------------------------------------------------------

void map_mono_to_stereo_duplicates() {
    const std::vector<float> input = {0.1F, -0.2F, 0.3F, 0.0F};
    std::vector<float> output(8, 9.0F);

    EXPECT_STATUS(dg_pcm_map_channels(view_of(input, 4, 1), buffer_of(output, 4, 2)), DG_OK);
    for (std::uint32_t frame = 0; frame < 4; ++frame) {
        CHECK(output[frame * 2] == input[frame]);
        CHECK(output[frame * 2 + 1] == input[frame]);
    }
}

void map_stereo_to_mono_averages() {
    const std::vector<float> input = {0.5F, -0.5F, 0.25F, 0.75F, 1.0F, 1.0F, -1.0F, 0.0F};
    std::vector<float> output(4, 9.0F);

    EXPECT_STATUS(dg_pcm_map_channels(view_of(input, 4, 2), buffer_of(output, 4, 1)), DG_OK);
    CHECK(output[0] == 0.0F);
    CHECK(output[1] == 0.5F);
    CHECK(output[2] == 1.0F);
    CHECK(output[3] == -0.5F);
}

void map_same_layout_is_bit_exact() {
    constexpr std::uint32_t kFrames = 128;
    const std::vector<float> stereo = make_samples(kFrames * 2, 0x77U);
    std::vector<float> stereo_out(stereo.size(), 0.0F);
    EXPECT_STATUS(dg_pcm_map_channels(view_of(stereo, kFrames, 2),
                                      buffer_of(stereo_out, kFrames, 2)),
                  DG_OK);
    CHECK(same_bits(stereo, stereo_out));

    const std::vector<float> mono = make_samples(kFrames, 0x88U);
    std::vector<float> mono_out(mono.size(), 0.0F);
    EXPECT_STATUS(dg_pcm_map_channels(view_of(mono, kFrames, 1), buffer_of(mono_out, kFrames, 1)),
                  DG_OK);
    CHECK(same_bits(mono, mono_out));
}

void map_rejects_bad_shapes() {
    float input[16] = {};
    float output[16] = {};

    // Frame counts must match even though the sample count might.
    EXPECT_STATUS(dg_pcm_map_channels(dg_pcm_view{input, 4, 1}, dg_pcm_buffer{output, 2, 2}),
                  DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_pcm_map_channels(dg_pcm_view{input, 4, 3}, dg_pcm_buffer{output, 4, 1}),
                  DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_pcm_map_channels(dg_pcm_view{input, 4, 1}, dg_pcm_buffer{output, 4, 0}),
                  DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_pcm_map_channels(dg_pcm_view{nullptr, 2, 1}, dg_pcm_buffer{output, 2, 1}),
                  DG_ERR_INVALID_ARGUMENT);
}

void map_same_layout_accepts_an_identical_buffer() {
    constexpr std::uint32_t kFrames = 48;
    std::vector<float> mono = make_samples(kFrames, 0xD00DU);
    const std::vector<float> mono_before = mono;
    EXPECT_STATUS(dg_pcm_map_channels(view_of(mono, kFrames, 1), buffer_of(mono, kFrames, 1)),
                  DG_OK);
    CHECK(same_bits(mono_before, mono));

    std::vector<float> stereo = make_samples(static_cast<std::size_t>(kFrames) * 2, 0xE00EU);
    const std::vector<float> stereo_before = stereo;
    EXPECT_STATUS(dg_pcm_map_channels(view_of(stereo, kFrames, 2), buffer_of(stereo, kFrames, 2)),
                  DG_OK);
    CHECK(same_bits(stereo_before, stereo));
}

void map_channel_conversion_rejects_aliasing() {
    std::vector<float> storage = make_samples(12, 0xF00DU);
    const std::vector<float> before = storage;
    float* base = storage.data();

    // mono -> stereo in place would overwrite the input it is still reading.
    EXPECT_STATUS(dg_pcm_map_channels(dg_pcm_view{base, 4, 1}, dg_pcm_buffer{base, 4, 2}),
                  DG_ERR_INVALID_ARGUMENT);
    CHECK(same_bits(before, storage));

    // Distinct pointers with intersecting ranges.
    EXPECT_STATUS(dg_pcm_map_channels(dg_pcm_view{base, 4, 1}, dg_pcm_buffer{base + 1, 4, 2}),
                  DG_ERR_INVALID_ARGUMENT);
    CHECK(same_bits(before, storage));

    // stereo -> mono in place is rejected as well: a conversion needs a distinct
    // buffer, and rejecting it keeps one rule for both directions.
    EXPECT_STATUS(dg_pcm_map_channels(dg_pcm_view{base, 4, 2}, dg_pcm_buffer{base, 4, 1}),
                  DG_ERR_INVALID_ARGUMENT);
    CHECK(same_bits(before, storage));
}

// ---------------------------------------------------------------------------
// dg_pcm_is_silent
// ---------------------------------------------------------------------------

void silence_is_detected() {
    const std::vector<float> silence = {0.0F, -0.0F, 0.0F, -0.0F};
    int silent = -1;
    EXPECT_STATUS(dg_pcm_is_silent(view_of(silence, 4, 1), &silent), DG_OK);
    CHECK(silent == 1);

    const std::vector<float> quiet_but_not_silent = {0.0F, 1.0e-30F};
    EXPECT_STATUS(dg_pcm_is_silent(view_of(quiet_but_not_silent, 2, 1), &silent), DG_OK);
    CHECK(silent == 0);

    const std::vector<float> audible = {0.5F, -0.5F};
    EXPECT_STATUS(dg_pcm_is_silent(view_of(audible, 1, 2), &silent), DG_OK);
    CHECK(silent == 0);
}

void silence_of_an_empty_block_is_true() {
    int silent = -1;
    EXPECT_STATUS(dg_pcm_is_silent(dg_pcm_view{nullptr, 0, 1}, &silent), DG_OK);
    CHECK(silent == 1);
    EXPECT_STATUS(dg_pcm_is_silent(dg_pcm_view{nullptr, 0, 2}, &silent), DG_OK);
    CHECK(silent == 1);
}

void silence_rejects_bad_arguments() {
    const std::vector<float> samples = {0.0F};
    int silent = -1;
    EXPECT_STATUS(dg_pcm_is_silent(view_of(samples, 1, 1), nullptr), DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_pcm_is_silent(dg_pcm_view{nullptr, 1, 1}, &silent), DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_pcm_is_silent(dg_pcm_view{samples.data(), 1, 3}, &silent),
                  DG_ERR_INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Cross-cutting properties
// ---------------------------------------------------------------------------

void large_block_round_trip() {
    // 65 536 stereo frames exercise the widened size_t index arithmetic with a
    // real allocation. The >2^31-frame case cannot be allocated in a test (it
    // needs multi-gigabyte buffers), so it stays a code-inspection property.
    constexpr std::uint32_t kFrames = 65536;
    const std::size_t samples = static_cast<std::size_t>(kFrames) * 2;
    const std::vector<float> input = make_samples(samples, 0x1234U);

    std::vector<float> bypassed(samples, 0.0F);
    EXPECT_STATUS(dg_pcm_bypass(view_of(input, kFrames, 2), buffer_of(bypassed, kFrames, 2)),
                  DG_OK);
    CHECK(same_bits(input, bypassed));

    std::vector<float> mono(kFrames, 0.0F);
    EXPECT_STATUS(dg_pcm_map_channels(view_of(input, kFrames, 2), buffer_of(mono, kFrames, 1)),
                  DG_OK);
    CHECK(mono[kFrames - 1] == (input[samples - 2] + input[samples - 1]) * 0.5F);
}

void bypass_is_the_identity_for_every_supported_frame_count() {
    const std::uint32_t frame_counts[] = {1, 2, 3, 63, 64, 65, 1024, 4096};
    const std::int32_t channel_counts[] = {1, 2};
    for (const std::uint32_t frames : frame_counts) {
        for (const std::int32_t channels : channel_counts) {
            const std::size_t samples = static_cast<std::size_t>(frames) *
                                        static_cast<std::size_t>(channels);
            const std::vector<float> input = make_samples(samples, frames * 31U + 7U);
            std::vector<float> output(samples, 0.0F);
            EXPECT_STATUS(dg_pcm_bypass(view_of(input, frames, channels),
                                        buffer_of(output, frames, channels)),
                          DG_OK);
            CHECK(same_bits(input, output));
        }
    }
}

void silent_input_produces_silent_output() {
    // Bypass and gain are linear, and the generator's silence test agrees.
    constexpr std::uint32_t kFrames = 96;
    const std::vector<float> silence(static_cast<std::size_t>(kFrames) * 2, 0.0F);
    std::vector<float> output(static_cast<std::size_t>(kFrames) * 2, 1.0F);
    EXPECT_STATUS(dg_pcm_bypass(view_of(silence, kFrames, 2), buffer_of(output, kFrames, 2)),
                  DG_OK);
    int silent = 0;
    EXPECT_STATUS(dg_pcm_is_silent(dg_pcm_view{output.data(), kFrames, 2}, &silent), DG_OK);
    CHECK(silent == 1);
}

#if SIZE_MAX <= 0xFFFFFFFFULL
void oversized_layout_is_rejected_on_32_bit_targets() {
    float sample = 0.0F;
    // 0xFFFFFFFF frames * 2 channels * 4 bytes does not fit a 32-bit size_t.
    EXPECT_STATUS(dg_pcm_bypass(dg_pcm_view{&sample, 0xFFFFFFFFU, 2},
                                dg_pcm_buffer{&sample, 0xFFFFFFFFU, 2}),
                  DG_ERR_INVALID_ARGUMENT);
}
#endif

}  // namespace

int main() {
    std::printf("dg_pcm_tests: portable Float32 PCM helpers (DG-011)\n");

    run("bypass preserves every bit (mono)", bypass_preserves_every_bit_mono);
    run("bypass preserves every bit (stereo)", bypass_preserves_every_bit_stereo);
    run("bypass keeps NaN bit patterns", bypass_keeps_nan_bit_patterns);
    run("bypass accepts an empty block", bypass_accepts_an_empty_block);
    run("bypass rejects bad descriptors", bypass_rejects_bad_descriptors);
    run("bypass accepts an identical buffer", bypass_accepts_an_identical_buffer);
    run("bypass rejects partial overlap", bypass_rejects_partial_overlap);
    run("gain 1.0 is bit exact for finite samples", gain_one_is_bit_exact_for_finite_samples);
    run("gain scales samples", gain_scales_samples);
    run("gain rejects out-of-contract values", gain_rejects_out_of_contract_values);
    run("gain accepts an identical buffer", gain_accepts_an_identical_buffer);
    run("gain rejects partial overlap", gain_rejects_partial_overlap);
    run("map mono to stereo duplicates", map_mono_to_stereo_duplicates);
    run("map stereo to mono averages", map_stereo_to_mono_averages);
    run("map same layout is bit exact", map_same_layout_is_bit_exact);
    run("map rejects bad shapes", map_rejects_bad_shapes);
    run("map same layout accepts an identical buffer", map_same_layout_accepts_an_identical_buffer);
    run("map channel conversion rejects aliasing", map_channel_conversion_rejects_aliasing);
    run("silence is detected", silence_is_detected);
    run("silence of an empty block is true", silence_of_an_empty_block_is_true);
    run("silence rejects bad arguments", silence_rejects_bad_arguments);
    run("bypass is the identity for every supported frame count",
        bypass_is_the_identity_for_every_supported_frame_count);
    run("large block round trip", large_block_round_trip);
    run("silent input produces silent output", silent_input_produces_silent_output);
#if SIZE_MAX <= 0xFFFFFFFFULL
    run("oversized layout is rejected on 32-bit targets",
        oversized_layout_is_rejected_on_32_bit_targets);
#endif

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures != 0) {
        std::printf("FAILED\n");
        return 1;
    }
    std::printf("PASSED\n");
    return 0;
}
