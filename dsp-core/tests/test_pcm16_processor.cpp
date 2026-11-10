// Tests for the DG-022 PCM16 <-> compressor bridge.
//
// Run on the host through CTest, same style as the other test programs.
//
// What these tests establish:
//   * create() validation: unsupported channel counts get the specific
//     channel error, bad compressor parameters are rejected by the wrapped
//     DG-013 validation, and a failed create clears its out-parameter;
//   * NULL handle/pointer handling and the zero-byte no-op;
//   * bypass is byte-exact, including a trailing partial frame/odd byte, and it
//     works in place;
//   * conversion is exact: with ratio 1 and no makeup gain every one of the
//     65 536 int16 values round-trips bit for bit;
//   * a -6.02 dB makeup gain halves even samples (within one LSB of float
//     rounding) and saturation clamps instead of wrapping;
//   * a constant full-scale block converges to the compressor's static curve
//     (threshold -20 dBFS, 2:1 -> -10 dB) and reset() restores unity gain;
//   * stereo linking attenuates a quiet channel together with a loud one while
//     unlinked processing leaves it alone;
//   * chunking the same stream produces bit-identical bytes, so the caller does
//     not have to negotiate a block size;
//   * a bypass block leaves the wrapped compressor's envelope exactly where it
//     was, so switching the Media3 toggle does not restart the dynamics;
//   * a partial frame at the end of the block is copied verbatim so the byte
//     count never changes; the same holds for a stereo partial frame and for a
//     partially-filled frame, and the reported frame count covers whole frames
//     only;
//   * undersized output, NULL pointers and partial overlap are rejected without
//     touching the output;
//   * the identical input/output pointer is allowed and matches separate buffers;
//   * a new handle can be created for a different rate/layout after destroying
//     the old one (format change);
//   * the bridge reports zero algorithmic latency.
//
// What these tests do NOT establish: no listening test, no device run, no
// true-peak behaviour (the bridge is not a limiter), and no claim that the
// chosen attack/release values sound like any particular processor. The "no
// allocation on the process path" property is enforced by code inspection (the
// only buffer is the scratch array inside the handle); a test cannot prove it.
//
// STATUS: UNVERIFIED. Written in rounds 22-23; the authoring environment could not
// execute commands, so none of these tests has been compiled or run.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "dreamguard/pcm16_processor.h"

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
#define EXPECT_STATUS(call, expected) expect_status(#call, (call), (expected), __LINE__)
#define EXPECT_CLOSE(actual, expected, tolerance) \
    expect_close((actual), (expected), (tolerance), #actual, __LINE__)

void run(const char* name, void (*test)()) {
    std::printf("  %s\n", name);
    test();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

constexpr double kMinusSixDb = -6.0205999132796239;  // 20 * log10(0.5)

dg_pcm16_processor_config config_for(std::int32_t sample_rate, std::int32_t channels,
                                     double threshold_db = -18.0, double ratio = 2.0,
                                     double knee_db = 6.0, double attack_ms = 20.0,
                                     double release_ms = 250.0, double makeup_db = 0.0,
                                     std::int32_t link_stereo = 1) {
    dg_pcm16_processor_config config{};
    config.sample_rate = sample_rate;
    config.channel_count = channels;
    config.threshold_db = threshold_db;
    config.ratio = ratio;
    config.knee_db = knee_db;
    config.attack_ms = attack_ms;
    config.release_ms = release_ms;
    config.makeup_gain_db = makeup_db;
    config.link_stereo = link_stereo;
    return config;
}

// A pass-through configuration: ratio 1 and zero makeup means the compressor's
// gain is exactly 1.0, so any output difference is a conversion bug.
dg_pcm16_processor_config unity_config(std::int32_t sample_rate, std::int32_t channels,
                                       double makeup_db = 0.0) {
    return config_for(sample_rate, channels, -20.0, 1.0, 0.0, 20.0, 250.0, makeup_db, 1);
}

std::vector<std::uint8_t> encode(const std::vector<std::int16_t>& samples) {
    std::vector<std::uint8_t> bytes(samples.size() * sizeof(std::int16_t), 0);
    if (!samples.empty()) {
        std::memcpy(bytes.data(), samples.data(), bytes.size());
    }
    return bytes;
}

std::vector<std::int16_t> decode(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::int16_t> samples(bytes.size() / sizeof(std::int16_t), 0);
    if (!samples.empty()) {
        std::memcpy(samples.data(), bytes.data(), samples.size() * sizeof(std::int16_t));
    }
    return samples;
}

// Deterministic material that includes the extremes a converter can get wrong.
std::vector<std::int16_t> make_samples(std::size_t count, std::uint32_t seed) {
    std::vector<std::int16_t> samples(count, 0);
    std::uint32_t state = seed;
    for (std::size_t index = 0; index < count; ++index) {
        if (index == 0) {
            samples[index] = std::numeric_limits<std::int16_t>::min();
            continue;
        }
        if (index == 1) {
            samples[index] = std::numeric_limits<std::int16_t>::max();
            continue;
        }
        state = state * 1664525U + 1013904223U;
        samples[index] = static_cast<std::int16_t>(static_cast<std::int32_t>(state >> 16) -
                                                   32768);
    }
    return samples;
}

dg_pcm16_processor* create_or_null(const dg_pcm16_processor_config& config) {
    dg_pcm16_processor* processor = nullptr;
    const dg_status status = dg_pcm16_processor_create(&config, &processor);
    if (status != DG_OK) {
        std::printf("    setup failed: create returned %d\n", static_cast<int>(status));
        return nullptr;
    }
    return processor;
}

dg_status process(dg_pcm16_processor* processor, const std::vector<std::uint8_t>& input,
                  std::vector<std::uint8_t>& output, std::int32_t bypass) {
    const auto capacity = static_cast<std::uint32_t>(output.size());
    return dg_pcm16_processor_process(processor, input.data(),
                                      static_cast<std::uint32_t>(input.size()),
                                      output.data(), capacity, bypass, nullptr);
}

// ---------------------------------------------------------------------------
// create / destroy / reset
// ---------------------------------------------------------------------------

void create_rejects_unsupported_channel_counts() {
    dg_pcm16_processor* processor = reinterpret_cast<dg_pcm16_processor*>(0x1);
    const dg_pcm16_processor_config three_channels = config_for(48000, 3);
    EXPECT_STATUS(dg_pcm16_processor_create(&three_channels, &processor),
                  DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE);
    CHECK(processor == nullptr);

    const dg_pcm16_processor_config zero_channels = config_for(48000, 0);
    EXPECT_STATUS(dg_pcm16_processor_create(&zero_channels, &processor),
                  DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE);
    CHECK(processor == nullptr);
}

void create_rejects_bad_compressor_parameters() {
    dg_pcm16_processor* processor = reinterpret_cast<dg_pcm16_processor*>(0x1);

    dg_pcm16_processor_config bad_rate = config_for(7999, 1);
    EXPECT_STATUS(dg_pcm16_processor_create(&bad_rate, &processor),
                  DG_ERR_SAMPLE_RATE_OUT_OF_RANGE);
    CHECK(processor == nullptr);

    dg_pcm16_processor_config bad_ratio = config_for(48000, 1);
    bad_ratio.ratio = 0.5;
    EXPECT_STATUS(dg_pcm16_processor_create(&bad_ratio, &processor),
                  DG_ERR_INVALID_ARGUMENT);
    CHECK(processor == nullptr);

    dg_pcm16_processor_config bad_link = config_for(48000, 2);
    bad_link.link_stereo = 2;
    EXPECT_STATUS(dg_pcm16_processor_create(&bad_link, &processor), DG_ERR_INVALID_ARGUMENT);
    CHECK(processor == nullptr);
}

void create_rejects_null_arguments() {
    dg_pcm16_processor* processor = nullptr;
    EXPECT_STATUS(dg_pcm16_processor_create(nullptr, &processor), DG_ERR_INVALID_ARGUMENT);
    CHECK(processor == nullptr);

    const dg_pcm16_processor_config config = config_for(48000, 2);
    EXPECT_STATUS(dg_pcm16_processor_create(&config, nullptr), DG_ERR_INVALID_ARGUMENT);
}

void create_accepts_every_supported_rate_and_layout() {
    const std::int32_t rates[] = {8000, 44100, 48000, 192000};
    const std::int32_t channels[] = {1, 2};
    for (const std::int32_t rate : rates) {
        for (const std::int32_t channel_count : channels) {
            dg_pcm16_processor* processor = create_or_null(unity_config(rate, channel_count));
            CHECK(processor != nullptr);
            if (processor != nullptr) {
                EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
            }
        }
    }
}

void null_handle_is_reported() {
    std::uint8_t byte = 0;
    EXPECT_STATUS(dg_pcm16_processor_destroy(nullptr), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_pcm16_processor_reset(nullptr), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_pcm16_processor_process(nullptr, &byte, 1, &byte, 1, 0, nullptr),
                  DG_ERR_NULL_HANDLE);
}

void zero_byte_call_is_a_no_op() {
    dg_pcm16_processor* processor = create_or_null(unity_config(48000, 1));
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }
    dg_pcm16_processor_values values{};
    values.frames = 9;
    values.bytes_written = 9;
    EXPECT_STATUS(dg_pcm16_processor_process(processor, nullptr, 0, nullptr, 0, 0, &values),
                  DG_OK);
    CHECK(values.frames == 0);
    CHECK(values.bytes_written == 0);
    CHECK(values.bypassed == 0);
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

// ---------------------------------------------------------------------------
// bypass
// ---------------------------------------------------------------------------

void bypass_is_byte_exact_including_a_partial_frame() {
    // Mono: two whole frames plus one odd byte.
    const std::vector<std::uint8_t> input = {0x34, 0x12, 0xFE, 0xFF, 0x7F};
    std::vector<std::uint8_t> output(input.size(), 0x00);
    dg_pcm16_processor* processor = create_or_null(config_for(48000, 1));
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }

    dg_pcm16_processor_values values{};
    EXPECT_STATUS(dg_pcm16_processor_process(
                      processor, input.data(), static_cast<std::uint32_t>(input.size()),
                      output.data(), static_cast<std::uint32_t>(output.size()), 1, &values),
                  DG_OK);
    CHECK(std::memcmp(input.data(), output.data(), input.size()) == 0);
    CHECK(values.frames == 2);
    CHECK(values.bytes_written == input.size());
    CHECK(values.bypassed == 1);
    CHECK(values.gain_reduction_db == 0.0);
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);

    // Stereo: one whole frame plus one leftover sample (two bytes).
    const std::vector<std::uint8_t> stereo = {0x01, 0x02, 0x03, 0x04, 0xAA, 0xBB};
    std::vector<std::uint8_t> stereo_output(stereo.size(), 0x00);
    processor = create_or_null(config_for(48000, 2));
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }
    EXPECT_STATUS(dg_pcm16_processor_process(
                      processor, stereo.data(), static_cast<std::uint32_t>(stereo.size()),
                      stereo_output.data(),
                      static_cast<std::uint32_t>(stereo_output.size()), 1, &values),
                  DG_OK);
    CHECK(std::memcmp(stereo.data(), stereo_output.data(), stereo.size()) == 0);
    CHECK(values.frames == 1);
    CHECK(values.bytes_written == stereo.size());
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

void bypass_accepts_the_identical_pointer() {
    std::vector<std::uint8_t> bytes = encode(make_samples(64, 0x1234U));
    const std::vector<std::uint8_t> before = bytes;
    dg_pcm16_processor* processor = create_or_null(unity_config(48000, 1));
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }
    EXPECT_STATUS(dg_pcm16_processor_process(processor, bytes.data(),
                                              static_cast<std::uint32_t>(bytes.size()),
                                              bytes.data(),
                                              static_cast<std::uint32_t>(bytes.size()), 1,
                                              nullptr),
                  DG_OK);
    CHECK(bytes == before);
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

// ---------------------------------------------------------------------------
// conversion and gain
// ---------------------------------------------------------------------------

void unity_gain_round_trips_every_int16_value() {
    dg_pcm16_processor* processor = create_or_null(unity_config(48000, 1));
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }

    std::vector<std::int16_t> input(65536, 0);
    for (std::size_t index = 0; index < input.size(); ++index) {
        input[index] = static_cast<std::int16_t>(static_cast<std::int32_t>(index) - 32768);
    }
    const std::vector<std::uint8_t> bytes = encode(input);
    std::vector<std::uint8_t> output(bytes.size(), 0x00);

    dg_pcm16_processor_values values{};
    EXPECT_STATUS(dg_pcm16_processor_process(
                      processor, bytes.data(), static_cast<std::uint32_t>(bytes.size()),
                      output.data(), static_cast<std::uint32_t>(output.size()), 0, &values),
                  DG_OK);
    CHECK(values.frames == 65536);
    CHECK(values.bytes_written == bytes.size());
    CHECK(values.bypassed == 0);
    CHECK(decode(output) == input);
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

void minus_six_db_makeup_halves_the_signal() {
    dg_pcm16_processor* processor = create_or_null(unity_config(48000, 1, kMinusSixDb));
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }

    const std::vector<std::int16_t> input = {0, 1, 2, 1000, 1001, 16384, -16384, 32767,
                                             -32768};
    const std::vector<std::uint8_t> bytes = encode(input);
    std::vector<std::uint8_t> output(bytes.size(), 0x00);
    EXPECT_STATUS(process(processor, bytes, output, 0), DG_OK);

    const std::vector<std::int16_t> decoded = decode(output);
    CHECK(decoded.size() == input.size());
    for (std::size_t index = 0; index < input.size() && index < decoded.size(); ++index) {
        const double expected =
            static_cast<double>(input[index]) * std::pow(10.0, kMinusSixDb / 20.0);
        if (std::fabs(expected - static_cast<double>(decoded[index])) > 1.0) {
            std::printf("    sample %zu: %d -> %d, expected about %.2f\n", index,
                        static_cast<int>(input[index]), static_cast<int>(decoded[index]),
                        expected);
            CHECK(false);
        }
    }
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

void saturation_clamps_instead_of_wrapping() {
    dg_pcm16_processor* processor = create_or_null(unity_config(48000, 1, 24.0));
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }

    const std::vector<std::int16_t> input = {32767, -32768, 16000, -16000};
    const std::vector<std::uint8_t> bytes = encode(input);
    std::vector<std::uint8_t> output(bytes.size(), 0x00);
    EXPECT_STATUS(process(processor, bytes, output, 0), DG_OK);

    const std::vector<std::int16_t> decoded = decode(output);
    CHECK(decoded.size() == 4);
    if (decoded.size() == 4) {
        CHECK(decoded[0] == 32767);
        CHECK(decoded[1] == -32768);
        CHECK(decoded[2] == 32767);
        CHECK(decoded[3] == -32768);
    }
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

// ---------------------------------------------------------------------------
// compression behaviour
// ---------------------------------------------------------------------------

void full_scale_block_converges_to_the_static_curve() {
    dg_pcm16_processor_config config = config_for(48000, 1, -20.0, 2.0, 0.0, 0.1, 500.0,
                                                  0.0, 1);
    dg_pcm16_processor* processor = create_or_null(config);
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }

    constexpr std::uint32_t kFrames = 48000;
    const std::vector<std::int16_t> input(kFrames, 32767);
    const std::vector<std::uint8_t> bytes = encode(input);
    std::vector<std::uint8_t> output(bytes.size(), 0x00);

    dg_pcm16_processor_values values{};
    EXPECT_STATUS(dg_pcm16_processor_process(
                      processor, bytes.data(), static_cast<std::uint32_t>(bytes.size()),
                      output.data(), static_cast<std::uint32_t>(output.size()), 0, &values),
                  DG_OK);

    const std::vector<std::int16_t> decoded = decode(output);
    // 0 dBFS through threshold -20 dBFS at 2:1 is -10 dB of reduction:
    // 32767 * 10^(-0.5) = 10362.
    const std::int16_t last = decoded.back();
    if (std::abs(static_cast<int>(last) - 10362) > 2) {
        std::printf("    settled sample %d, expected about 10362\n",
                    static_cast<int>(last));
        CHECK(false);
    }
    CHECK(last < 32767);
    EXPECT_CLOSE(values.gain_reduction_db, -10.0, 0.05);
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

void reset_returns_the_compressor_to_unity() {
    dg_pcm16_processor* processor = create_or_null(
        config_for(48000, 1, -20.0, 2.0, 0.0, 0.1, 500.0, 0.0, 1));
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }

    // Loud block: build up gain reduction.
    const std::vector<std::int16_t> loud(4800, 32767);
    std::vector<std::uint8_t> output(encode(loud).size(), 0x00);
    EXPECT_STATUS(process(processor, encode(loud), output, 0), DG_OK);

    EXPECT_STATUS(dg_pcm16_processor_reset(processor), DG_OK);

    // A quiet block is below the threshold, so with a fresh gain state the
    // output is exact: no reduction, no makeup gain.
    const std::vector<std::int16_t> quiet(64, 1000);
    const std::vector<std::uint8_t> quiet_bytes = encode(quiet);
    std::vector<std::uint8_t> quiet_output(quiet_bytes.size(), 0x00);
    dg_pcm16_processor_values values{};
    EXPECT_STATUS(dg_pcm16_processor_process(
                      processor, quiet_bytes.data(),
                      static_cast<std::uint32_t>(quiet_bytes.size()),
                      quiet_output.data(),
                      static_cast<std::uint32_t>(quiet_output.size()), 0, &values),
                  DG_OK);
    CHECK(decode(quiet_output) == quiet);
    EXPECT_CLOSE(values.gain_reduction_db, 0.0, 1.0e-9);
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

void linked_channels_share_one_gain() {
    dg_pcm16_processor_config linked = config_for(48000, 2, -20.0, 2.0, 0.0, 0.1, 500.0,
                                                  0.0, 1);
    dg_pcm16_processor* processor = create_or_null(linked);
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }

    constexpr std::uint32_t kFrames = 1024;
    std::vector<std::int16_t> input(kFrames * 2);
    for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
        input[frame * 2] = 32767;
        input[frame * 2 + 1] = 1000;
    }
    const std::vector<std::uint8_t> bytes = encode(input);
    std::vector<std::uint8_t> output(bytes.size(), 0x00);
    EXPECT_STATUS(process(processor, bytes, output, 0), DG_OK);

    const std::vector<std::int16_t> decoded = decode(output);
    const std::int16_t left = decoded[(kFrames - 1) * 2];
    const std::int16_t right = decoded[(kFrames - 1) * 2 + 1];
    CHECK(left < 12000);
    // The quiet channel is pulled down by the loud one instead of staying at
    // 1000: that is what "linked" means.
    CHECK(right < 700);
    CHECK(right > 0);
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

void unlinked_channels_are_independent() {
    dg_pcm16_processor_config unlinked = config_for(48000, 2, -20.0, 2.0, 0.0, 0.1, 500.0,
                                                    0.0, 0);
    dg_pcm16_processor* processor = create_or_null(unlinked);
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }

    constexpr std::uint32_t kFrames = 1024;
    std::vector<std::int16_t> input(kFrames * 2);
    for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
        input[frame * 2] = 32767;
        input[frame * 2 + 1] = 1000;
    }
    const std::vector<std::uint8_t> bytes = encode(input);
    std::vector<std::uint8_t> output(bytes.size(), 0x00);
    EXPECT_STATUS(process(processor, bytes, output, 0), DG_OK);

    const std::vector<std::int16_t> decoded = decode(output);
    const std::int16_t left = decoded[(kFrames - 1) * 2];
    const std::int16_t right = decoded[(kFrames - 1) * 2 + 1];
    CHECK(left < 12000);
    // 1000 / 32768 is about -30 dBFS: below the threshold, so no reduction.
    CHECK(right == 1000);
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

// ---------------------------------------------------------------------------
// buffering and stream properties
// ---------------------------------------------------------------------------

void chunking_does_not_change_the_bytes() {
    const std::vector<std::int16_t> samples = make_samples(5000 * 2, 0xCAFEU);
    const std::vector<std::uint8_t> bytes = encode(samples);

    dg_pcm16_processor_config config = config_for(48000, 2, -24.0, 3.0, 4.0, 5.0, 120.0,
                                                  0.0, 1);
    dg_pcm16_processor* whole = create_or_null(config);
    dg_pcm16_processor* split = create_or_null(config);
    CHECK(whole != nullptr);
    CHECK(split != nullptr);
    if (whole == nullptr || split == nullptr) {
        if (whole != nullptr) {
            EXPECT_STATUS(dg_pcm16_processor_destroy(whole), DG_OK);
        }
        if (split != nullptr) {
            EXPECT_STATUS(dg_pcm16_processor_destroy(split), DG_OK);
        }
        return;
    }

    std::vector<std::uint8_t> whole_output(bytes.size(), 0x00);
    EXPECT_STATUS(process(whole, bytes, whole_output, 0), DG_OK);

    std::vector<std::uint8_t> split_output(bytes.size(), 0x00);
    const std::size_t frame_bytes = 2 * sizeof(std::int16_t);
    const std::size_t chunk_frames[] = {1, 2, 499, 1000, 2048, 5000};
    std::size_t frame_offset = 0;
    std::size_t chunk_index = 0;
    while (frame_offset < 5000) {
        const std::size_t frames =
            std::min<std::size_t>(chunk_frames[chunk_index % 6], 5000 - frame_offset);
        const std::size_t byte_offset = frame_offset * frame_bytes;
        const std::size_t byte_count = frames * frame_bytes;
        const dg_status status = dg_pcm16_processor_process(
            split, bytes.data() + byte_offset, static_cast<std::uint32_t>(byte_count),
            split_output.data() + byte_offset, static_cast<std::uint32_t>(byte_count), 0,
            nullptr);
        EXPECT_STATUS(status, DG_OK);
        if (status != DG_OK) {
            break;
        }
        frame_offset += frames;
        ++chunk_index;
    }

    CHECK(whole_output == split_output);
    EXPECT_STATUS(dg_pcm16_processor_destroy(whole), DG_OK);
    EXPECT_STATUS(dg_pcm16_processor_destroy(split), DG_OK);
}

void partial_frames_are_copied_verbatim() {
    dg_pcm16_processor* processor = create_or_null(unity_config(48000, 1));
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }

    // Two complete mono frames (0x0001, 0x0002) plus one odd byte 0x7F.
    const std::vector<std::uint8_t> input = {0x01, 0x00, 0x02, 0x00, 0x7F};
    std::vector<std::uint8_t> output(input.size(), 0x00);
    dg_pcm16_processor_values values{};
    EXPECT_STATUS(dg_pcm16_processor_process(
                      processor, input.data(), static_cast<std::uint32_t>(input.size()),
                      output.data(), static_cast<std::uint32_t>(output.size()), 1, &values),
                  DG_OK);
    CHECK(output.back() == 0x7F);
    CHECK(values.frames == 2);
    CHECK(values.bytes_written == input.size());

    // In processing mode the conversion covers the whole frames and the odd
    // byte is still copied.
    std::vector<std::uint8_t> processed(input.size(), 0x00);
    EXPECT_STATUS(dg_pcm16_processor_process(
                      processor, input.data(), static_cast<std::uint32_t>(input.size()),
                      processed.data(), static_cast<std::uint32_t>(processed.size()), 0,
                      &values),
                  DG_OK);
    CHECK(processed.size() == input.size());
    CHECK(processed.back() == 0x7F);
    CHECK(decode(processed) == decode(input));  // unity gain
    CHECK(values.frames == 2);
    CHECK(values.bytes_written == input.size());
    CHECK(values.bypassed == 0);
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

void stereo_partial_frame_is_preserved() {
    dg_pcm16_processor* processor = create_or_null(unity_config(48000, 2));
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }

    // One whole stereo frame (two samples) plus one leftover sample: two bytes
    // that do not fill a frame. The frame count must cover only the whole frame,
    // while both the converted frame and the raw tail keep their bytes.
    const std::vector<std::int16_t> input = {100, -100, 200};
    const std::vector<std::uint8_t> bytes = encode(input);
    std::vector<std::uint8_t> output(bytes.size(), 0x5A);

    dg_pcm16_processor_values values{};
    EXPECT_STATUS(dg_pcm16_processor_process(
                      processor, bytes.data(), static_cast<std::uint32_t>(bytes.size()),
                      output.data(), static_cast<std::uint32_t>(output.size()), 0, &values),
                  DG_OK);
    CHECK(values.frames == 1);
    CHECK(values.bytes_written == bytes.size());
    CHECK(values.bypassed == 0);

    const std::vector<std::int16_t> decoded = decode(output);
    CHECK(decoded.size() == 3);
    if (decoded.size() == 3) {
        CHECK(decoded[0] == 100);
        CHECK(decoded[1] == -100);
        // The leftover sample is copied verbatim; a converter that rounded it
        // away (or wrote only whole frames) would fail here.
        CHECK(std::memcmp(output.data() + 4, bytes.data() + 4, 2) == 0);
        CHECK(decoded[2] == 200);
    }
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

void larger_output_capacity_is_accepted() {
    dg_pcm16_processor* processor = create_or_null(unity_config(48000, 1));
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }
    const std::vector<std::int16_t> input = {1, -2, 3, -4};
    const std::vector<std::uint8_t> bytes = encode(input);
    // The JNI bridge can hand the bridge a capacity larger than the block; only
    // the input byte count may be written and reported.
    std::vector<std::uint8_t> output(bytes.size() + 16, 0x5A);
    dg_pcm16_processor_values values{};
    EXPECT_STATUS(dg_pcm16_processor_process(
                      processor, bytes.data(), static_cast<std::uint32_t>(bytes.size()),
                      output.data(), static_cast<std::uint32_t>(output.size()), 0, &values),
                  DG_OK);
    CHECK(values.bytes_written == bytes.size());
    const std::vector<std::uint8_t> converted(output.begin(),
                                              output.begin() + bytes.size());
    CHECK(decode(converted) == input);
    for (std::size_t index = bytes.size(); index < output.size(); ++index) {
        CHECK(output[index] == 0x5A);
    }
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

void bypass_does_not_touch_the_compressor_state() {
    // Both handles process the same loud block, then one of them passes a block
    // through bypass while the other does nothing. Their next processed block
    // must be byte-identical: a bypass that reset or advanced the envelope would
    // show up as a different gain on the quiet block.
    dg_pcm16_processor_config config =
        config_for(48000, 1, -20.0, 2.0, 0.0, 0.1, 500.0, 0.0, 1);
    dg_pcm16_processor* bypassing = create_or_null(config);
    dg_pcm16_processor* untouched = create_or_null(config);
    CHECK(bypassing != nullptr);
    CHECK(untouched != nullptr);
    if (bypassing == nullptr || untouched == nullptr) {
        if (bypassing != nullptr) {
            EXPECT_STATUS(dg_pcm16_processor_destroy(bypassing), DG_OK);
        }
        if (untouched != nullptr) {
            EXPECT_STATUS(dg_pcm16_processor_destroy(untouched), DG_OK);
        }
        return;
    }

    const std::vector<std::int16_t> loud(4800, 32767);
    const std::vector<std::uint8_t> loud_bytes = encode(loud);
    std::vector<std::uint8_t> scratch(loud_bytes.size(), 0x00);
    EXPECT_STATUS(process(bypassing, loud_bytes, scratch, 0), DG_OK);
    EXPECT_STATUS(process(untouched, loud_bytes, scratch, 0), DG_OK);

    std::vector<std::uint8_t> bypassed(loud_bytes.size(), 0x00);
    dg_pcm16_processor_values values{};
    EXPECT_STATUS(dg_pcm16_processor_process(
                      bypassing, loud_bytes.data(),
                      static_cast<std::uint32_t>(loud_bytes.size()), bypassed.data(),
                      static_cast<std::uint32_t>(bypassed.size()), 1, &values),
                  DG_OK);
    CHECK(bypassed == loud_bytes);
    CHECK(values.bypassed == 1);
    CHECK(values.gain_reduction_db == 0.0);

    const std::vector<std::int16_t> quiet(64, 1000);
    const std::vector<std::uint8_t> quiet_bytes = encode(quiet);
    std::vector<std::uint8_t> bypassing_output(quiet_bytes.size(), 0x00);
    std::vector<std::uint8_t> untouched_output(quiet_bytes.size(), 0x00);
    EXPECT_STATUS(process(bypassing, quiet_bytes, bypassing_output, 0), DG_OK);
    EXPECT_STATUS(process(untouched, quiet_bytes, untouched_output, 0), DG_OK);
    CHECK(bypassing_output == untouched_output);
    // Prove the block was meaningful: the residual -10 dB reduction from the
    // loud block is still being released, so unity output would be a bug.
    CHECK(decode(bypassing_output) != quiet);

    EXPECT_STATUS(dg_pcm16_processor_destroy(bypassing), DG_OK);
    EXPECT_STATUS(dg_pcm16_processor_destroy(untouched), DG_OK);
}

void processing_does_not_modify_a_separate_input() {
    const std::vector<std::int16_t> samples = make_samples(256, 0xBEEFU);
    const std::vector<std::uint8_t> bytes = encode(samples);
    const std::vector<std::uint8_t> before = bytes;
    std::vector<std::uint8_t> output(bytes.size(), 0x00);
    dg_pcm16_processor* processor = create_or_null(unity_config(48000, 1));
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }
    EXPECT_STATUS(process(processor, bytes, output, 0), DG_OK);
    CHECK(bytes == before);
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

// ---------------------------------------------------------------------------
// argument and aliasing contract
// ---------------------------------------------------------------------------

void undersized_output_is_rejected_without_writing() {
    dg_pcm16_processor* processor = create_or_null(unity_config(48000, 1));
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }
    const std::vector<std::uint8_t> input = encode(std::vector<std::int16_t>{1, 2, 3, 4});
    std::vector<std::uint8_t> output(input.size(), 0x5A);
    EXPECT_STATUS(dg_pcm16_processor_process(
                      processor, input.data(), static_cast<std::uint32_t>(input.size()),
                      output.data(), static_cast<std::uint32_t>(output.size() - 1), 0,
                      nullptr),
                  DG_ERR_INVALID_ARGUMENT);
    for (const std::uint8_t byte : output) {
        CHECK(byte == 0x5A);
    }
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

void null_buffers_are_rejected_when_bytes_are_nonzero() {
    dg_pcm16_processor* processor = create_or_null(unity_config(48000, 1));
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }
    std::uint8_t storage[4] = {};
    EXPECT_STATUS(dg_pcm16_processor_process(processor, nullptr, 2, storage, 2, 0, nullptr),
                  DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_pcm16_processor_process(processor, storage, 2, nullptr, 2, 0, nullptr),
                  DG_ERR_INVALID_ARGUMENT);
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

void partial_overlap_is_rejected() {
    dg_pcm16_processor* processor = create_or_null(unity_config(48000, 1));
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }
    std::uint8_t storage[8] = {0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x00, 0x00};
    const std::uint8_t before[8] = {0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x00, 0x00};
    EXPECT_STATUS(dg_pcm16_processor_process(processor, storage, 6, storage + 1, 6, 0,
                                              nullptr),
                  DG_ERR_INVALID_ARGUMENT);
    CHECK(std::memcmp(storage, before, sizeof(before)) == 0);
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

void in_place_processing_matches_separate_buffers() {
    const std::vector<std::int16_t> samples = make_samples(1024, 0x0F0FU);
    const std::vector<std::uint8_t> bytes = encode(samples);

    dg_pcm16_processor* processor = create_or_null(unity_config(48000, 1, kMinusSixDb));
    CHECK(processor != nullptr);
    if (processor == nullptr) {
        return;
    }
    std::vector<std::uint8_t> separate(bytes.size(), 0x00);
    EXPECT_STATUS(process(processor, bytes, separate, 0), DG_OK);

    std::vector<std::uint8_t> in_place = bytes;
    EXPECT_STATUS(dg_pcm16_processor_process(
                      processor, in_place.data(), static_cast<std::uint32_t>(in_place.size()),
                      in_place.data(), static_cast<std::uint32_t>(in_place.size()), 0,
                      nullptr),
                  DG_OK);
    CHECK(in_place == separate);
    EXPECT_STATUS(dg_pcm16_processor_destroy(processor), DG_OK);
}

// ---------------------------------------------------------------------------
// format change and latency
// ---------------------------------------------------------------------------

void a_new_format_gets_a_fresh_handle() {
    dg_pcm16_processor* mono = create_or_null(unity_config(44100, 1));
    CHECK(mono != nullptr);
    if (mono == nullptr) {
        return;
    }
    const std::vector<std::int16_t> mono_samples = {100, -100, 32767, -32768};
    const std::vector<std::uint8_t> mono_bytes = encode(mono_samples);
    std::vector<std::uint8_t> mono_output(mono_bytes.size(), 0x00);
    EXPECT_STATUS(process(mono, mono_bytes, mono_output, 0), DG_OK);
    CHECK(decode(mono_output) == mono_samples);
    EXPECT_STATUS(dg_pcm16_processor_destroy(mono), DG_OK);

    dg_pcm16_processor* stereo = create_or_null(unity_config(48000, 2));
    CHECK(stereo != nullptr);
    if (stereo == nullptr) {
        return;
    }
    const std::vector<std::int16_t> stereo_samples = {100, 200, -300, -400};
    const std::vector<std::uint8_t> stereo_bytes = encode(stereo_samples);
    std::vector<std::uint8_t> stereo_output(stereo_bytes.size(), 0x00);
    EXPECT_STATUS(process(stereo, stereo_bytes, stereo_output, 0), DG_OK);
    CHECK(decode(stereo_output) == stereo_samples);
    EXPECT_STATUS(dg_pcm16_processor_destroy(stereo), DG_OK);
}

void latency_is_zero() {
    CHECK(dg_pcm16_processor_latency_frames() == 0);
}

}  // namespace

int main() {
    std::printf("dg_pcm16_processor_tests: PCM16 <-> DG-013 compressor bridge (DG-022)\n");

    run("create rejects unsupported channel counts",
        create_rejects_unsupported_channel_counts);
    run("create rejects bad compressor parameters",
        create_rejects_bad_compressor_parameters);
    run("create rejects null arguments", create_rejects_null_arguments);
    run("create accepts every supported rate and layout",
        create_accepts_every_supported_rate_and_layout);
    run("null handle is reported", null_handle_is_reported);
    run("zero byte call is a no-op", zero_byte_call_is_a_no_op);
    run("bypass is byte exact including a partial frame",
        bypass_is_byte_exact_including_a_partial_frame);
    run("bypass accepts the identical pointer", bypass_accepts_the_identical_pointer);
    run("unity gain round trips every int16 value",
        unity_gain_round_trips_every_int16_value);
    run("minus six dB makeup halves the signal", minus_six_db_makeup_halves_the_signal);
    run("saturation clamps instead of wrapping", saturation_clamps_instead_of_wrapping);
    run("full scale block converges to the static curve",
        full_scale_block_converges_to_the_static_curve);
    run("reset returns the compressor to unity", reset_returns_the_compressor_to_unity);
    run("linked channels share one gain", linked_channels_share_one_gain);
    run("unlinked channels are independent", unlinked_channels_are_independent);
    run("chunking does not change the bytes", chunking_does_not_change_the_bytes);
    run("partial frames are copied verbatim", partial_frames_are_copied_verbatim);
    run("a stereo partial frame is preserved", stereo_partial_frame_is_preserved);
    run("a larger output capacity is accepted", larger_output_capacity_is_accepted);
    run("bypass does not touch the compressor state",
        bypass_does_not_touch_the_compressor_state);
    run("processing does not modify a separate input",
        processing_does_not_modify_a_separate_input);
    run("undersized output is rejected without writing",
        undersized_output_is_rejected_without_writing);
    run("null buffers are rejected when bytes are nonzero",
        null_buffers_are_rejected_when_bytes_are_nonzero);
    run("partial overlap is rejected", partial_overlap_is_rejected);
    run("in place processing matches separate buffers",
        in_place_processing_matches_separate_buffers);
    run("a new format gets a fresh handle", a_new_format_gets_a_fresh_handle);
    run("latency is zero", latency_is_zero);

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures != 0) {
        std::printf("FAILED\n");
        return 1;
    }
    std::printf("PASSED\n");
    return 0;
}
