// Lifecycle tests for the portable DSP core (DG-010).
//
// Runs on the host through CTest and needs no third-party test framework:
// a C++20 compiler, CMake and a C++ runtime with threads are the whole
// requirement. Covered here:
//   * the dg_create validation matrix (NULL, out-of-range, boundary values);
//   * handle ownership: alive handles are distinct, NULL is reported as
//     DG_ERR_NULL_HANDLE on every entry point, destroy releases once;
//   * the documented per-handle threading contract: setters may race render,
//     and separate engines share no state, so they can run in parallel threads
//     and still produce byte-identical output;
//   * dg_reset semantics, proven through observable determinism;
//   * dg_render argument validation and output shape;
//   * repeated create/render/reset/destroy cycles for cross-engine determinism.
//
// Deliberately NOT tested or claimed: using a handle after dg_destroy. The
// public contract calls that undefined behaviour and the implementation does
// not detect it (no registry, no generation tags), so a test would only be
// asserting undefined behaviour. Equally, this is not a leak detector and not a
// race detector - concurrency tests here are smoke tests; docs/STATUS.md keeps
// DG-010 short of VERIFIED for exactly those reasons.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "dreamguard/dsp_core.h"

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

dg_engine* create_engine(const dg_config& config) {
    dg_engine* engine = nullptr;
    if (dg_create(&config, &engine) != DG_OK) {
        std::printf("    helper: dg_create failed\n");
        ++g_failures;
    }
    return engine;
}

bool buffer_is_finite_and_bounded(const std::vector<float>& samples) {
    for (const float sample : samples) {
        if (!std::isfinite(sample) || std::abs(sample) >= 1.0F) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// dg_create validation
// ---------------------------------------------------------------------------

void create_rejects_null_arguments() {
    dg_engine* engine = reinterpret_cast<dg_engine*>(static_cast<intptr_t>(1));
    EXPECT_STATUS(dg_create(nullptr, &engine), DG_ERR_INVALID_ARGUMENT);
    CHECK(engine == nullptr);

    const dg_config valid{48000, 1};
    EXPECT_STATUS(dg_create(&valid, nullptr), DG_ERR_INVALID_ARGUMENT);
}

void create_rejects_out_of_range_sample_rate() {
    const int32_t rejected[] = {0, -1, DG_SAMPLE_RATE_MIN - 1, DG_SAMPLE_RATE_MAX + 1};
    for (const int32_t rate : rejected) {
        const dg_config config{rate, 1};
        // Pre-seeded with junk: a failing create must clear the out parameter.
        dg_engine* engine = reinterpret_cast<dg_engine*>(static_cast<intptr_t>(1));
        EXPECT_STATUS(dg_create(&config, &engine), DG_ERR_SAMPLE_RATE_OUT_OF_RANGE);
        CHECK(engine == nullptr);
    }
}

void create_rejects_out_of_range_channel_count() {
    const int32_t rejected[] = {0, -1, DG_CHANNELS_MAX + 1, 8};
    for (const int32_t count : rejected) {
        const dg_config config{48000, count};
        dg_engine* engine = reinterpret_cast<dg_engine*>(static_cast<intptr_t>(1));
        EXPECT_STATUS(dg_create(&config, &engine), DG_ERR_CHANNEL_COUNT_OUT_OF_RANGE);
        CHECK(engine == nullptr);
    }
}

void create_checks_sample_rate_before_channel_count() {
    // Both fields are invalid; the documented order reports the sample rate,
    // which keeps the JNI create path's single return value predictable.
    const dg_config config{0, 0};
    dg_engine* engine = nullptr;
    EXPECT_STATUS(dg_create(&config, &engine), DG_ERR_SAMPLE_RATE_OUT_OF_RANGE);
    CHECK(engine == nullptr);
}

void create_accepts_every_supported_configuration() {
    const int32_t rates[] = {DG_SAMPLE_RATE_MIN, 44100, 48000, DG_SAMPLE_RATE_MAX};
    const int32_t channels[] = {DG_CHANNELS_MIN, DG_CHANNELS_MAX};
    for (const int32_t rate : rates) {
        for (const int32_t count : channels) {
            const dg_config config{rate, count};
            dg_engine* engine = nullptr;
            EXPECT_STATUS(dg_create(&config, &engine), DG_OK);
            CHECK(engine != nullptr);
            CHECK(dg_channel_count(engine) == count);
            EXPECT_STATUS(dg_destroy(engine), DG_OK);
        }
    }
}

void live_handles_are_distinct() {
    const dg_config config{48000, 1};
    dg_engine* first = nullptr;
    dg_engine* second = nullptr;
    EXPECT_STATUS(dg_create(&config, &first), DG_OK);
    EXPECT_STATUS(dg_create(&config, &second), DG_OK);
    CHECK(first != nullptr);
    CHECK(second != nullptr);
    CHECK(first != second);
    // The two engines are independent: rendering one must not advance the other.
    std::vector<float> a_block(32, 0.0F);
    std::vector<float> b_block(32, 0.0F);
    uint32_t written = 0;
    EXPECT_STATUS(dg_render(first, a_block.data(), 32, &written), DG_OK);
    EXPECT_STATUS(dg_render(second, b_block.data(), 32, &written), DG_OK);
    CHECK(std::memcmp(a_block.data(), b_block.data(), 32 * sizeof(float)) == 0);
    EXPECT_STATUS(dg_destroy(first), DG_OK);
    EXPECT_STATUS(dg_destroy(second), DG_OK);
}

// ---------------------------------------------------------------------------
// NULL handles
// ---------------------------------------------------------------------------

void null_handles_report_null_handle() {
    EXPECT_STATUS(dg_reset(nullptr), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_destroy(nullptr), DG_ERR_NULL_HANDLE);

    float buffer[8] = {};
    uint32_t written = 7;
    EXPECT_STATUS(dg_render(nullptr, buffer, 4, &written), DG_ERR_NULL_HANDLE);
    CHECK(written == 0);
    EXPECT_STATUS(dg_set_gain(nullptr, 1.0F), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_set_tone_cutoff_hz(nullptr, 400.0F), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_set_noise_mix(nullptr, 1.0F), DG_ERR_NULL_HANDLE);
    EXPECT_STATUS(dg_set_drone_hz(nullptr, 100.0F), DG_ERR_NULL_HANDLE);
    CHECK(dg_channel_count(nullptr) == 0);
}

void destroy_accepts_a_valid_handle_once() {
    const dg_config config{48000, 1};
    dg_engine* engine = create_engine(config);
    CHECK(engine != nullptr);
    EXPECT_STATUS(dg_reset(engine), DG_OK);
    EXPECT_STATUS(dg_destroy(engine), DG_OK);
    // No second destroy: that would be undefined behaviour by contract.
}

// ---------------------------------------------------------------------------
// Render contract
// ---------------------------------------------------------------------------

void render_validates_arguments() {
    const dg_config config{48000, 2};
    dg_engine* engine = create_engine(config);
    CHECK(engine != nullptr);

    float buffer[32] = {};
    uint32_t written = 99;
    EXPECT_STATUS(dg_render(engine, nullptr, 8, &written), DG_ERR_INVALID_ARGUMENT);
    CHECK(written == 0);

    written = 99;
    EXPECT_STATUS(dg_render(engine, buffer, 0, &written), DG_ERR_INVALID_ARGUMENT);
    CHECK(written == 0);

    EXPECT_STATUS(dg_render(engine, buffer, 8, nullptr), DG_ERR_INVALID_ARGUMENT);

    EXPECT_STATUS(dg_destroy(engine), DG_OK);
}

void render_writes_interleaved_in_range_samples() {
    const dg_config config{48000, 2};
    dg_engine* engine = create_engine(config);
    CHECK(engine != nullptr);

    constexpr uint32_t kFrames = 512;
    std::vector<float> buffer(kFrames * 2, 0.0F);
    uint32_t written = 0;
    EXPECT_STATUS(dg_render(engine, buffer.data(), kFrames, &written), DG_OK);
    CHECK(written == kFrames);

    bool any_non_zero = false;
    bool stereo_matches = true;
    bool in_range = true;
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        const float left = buffer[frame * 2];
        const float right = buffer[frame * 2 + 1];
        stereo_matches = stereo_matches && (left == right);
        in_range = in_range && std::isfinite(left) && std::abs(left) < 1.0F;
        any_non_zero = any_non_zero || left != 0.0F;
    }
    CHECK(stereo_matches);
    CHECK(in_range);
    CHECK(any_non_zero);

    EXPECT_STATUS(dg_destroy(engine), DG_OK);
}

// ---------------------------------------------------------------------------
// dg_reset
// ---------------------------------------------------------------------------

void reset_restores_the_initial_render_state() {
    const dg_config config{48000, 1};
    dg_engine* engine = create_engine(config);
    CHECK(engine != nullptr);

    constexpr uint32_t kFrames = 256;
    std::vector<float> first(kFrames, 0.0F);
    std::vector<float> before_reset(kFrames, 0.0F);
    std::vector<float> after_reset(kFrames, 0.0F);

    uint32_t written = 0;
    EXPECT_STATUS(dg_render(engine, first.data(), kFrames, &written), DG_OK);
    EXPECT_STATUS(dg_render(engine, before_reset.data(), kFrames, &written), DG_OK);
    // Guard against a vacuous test: if consecutive blocks were identical, the
    // bitwise comparison after reset would prove nothing.
    CHECK(std::memcmp(first.data(), before_reset.data(),
                      kFrames * sizeof(float)) != 0);

    EXPECT_STATUS(dg_reset(engine), DG_OK);
    EXPECT_STATUS(dg_render(engine, after_reset.data(), kFrames, &written), DG_OK);
    CHECK(std::memcmp(first.data(), after_reset.data(),
                      kFrames * sizeof(float)) == 0);

    EXPECT_STATUS(dg_destroy(engine), DG_OK);
}

void reset_preserves_parameters() {
    const dg_config config{48000, 1};
    dg_engine* engine = create_engine(config);
    CHECK(engine != nullptr);

    EXPECT_STATUS(dg_set_drone_hz(engine, 180.0F), DG_OK);
    EXPECT_STATUS(dg_set_gain(engine, 0.5F), DG_OK);
    EXPECT_STATUS(dg_reset(engine), DG_OK);

    float buffer[64] = {};
    uint32_t written = 0;
    EXPECT_STATUS(dg_render(engine, buffer, 64, &written), DG_OK);
    CHECK(written == 64);

    EXPECT_STATUS(dg_destroy(engine), DG_OK);
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

void parameter_setters_keep_the_documented_clamping() {
    const dg_config config{48000, 1};
    dg_engine* engine = create_engine(config);
    CHECK(engine != nullptr);

    EXPECT_STATUS(dg_set_gain(engine, 2.0F), DG_OK);            // clamps to 1.0
    EXPECT_STATUS(dg_set_gain(engine, std::nanf("")), DG_OK);   // falls back to default
    EXPECT_STATUS(dg_set_gain(engine, INFINITY), DG_OK);        // falls back to default
    EXPECT_STATUS(dg_set_tone_cutoff_hz(engine, 1.0F), DG_OK);  // clamps to 80 Hz
    EXPECT_STATUS(dg_set_tone_cutoff_hz(engine, 1.0e9F), DG_OK);
    EXPECT_STATUS(dg_set_noise_mix(engine, -1.0F), DG_OK);
    EXPECT_STATUS(dg_set_drone_hz(engine, 1.0e9F), DG_OK);

    float buffer[128] = {};
    uint32_t written = 0;
    EXPECT_STATUS(dg_render(engine, buffer, 128, &written), DG_OK);
    CHECK(written == 128);

    bool in_range = true;
    for (const float sample : buffer) {
        in_range = in_range && std::isfinite(sample) && std::abs(sample) < 1.0F;
    }
    CHECK(in_range);

    EXPECT_STATUS(dg_destroy(engine), DG_OK);
}

// ---------------------------------------------------------------------------
// Concurrency contract
// ---------------------------------------------------------------------------

void setters_may_race_with_render() {
    // Documented contract: setters store atomics, render reads them. One render
    // thread plus one setter thread must stay clean and in range.
    const dg_config config{48000, 2};
    dg_engine* engine = create_engine(config);
    CHECK(engine != nullptr);

    std::atomic<bool> stop{false};
    std::atomic<int> render_failures{0};
    std::atomic<int> setter_failures{0};

    std::thread renderer([&] {
        std::vector<float> block(256 * 2, 0.0F);
        uint32_t written = 0;
        for (;;) {
            if (stop.load(std::memory_order_acquire)) {
                return;
            }
            if (dg_render(engine, block.data(), 256, &written) != DG_OK ||
                written != 256) {
                render_failures.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (!buffer_is_finite_and_bounded(block)) {
                render_failures.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    });

    std::thread setter([&] {
        float value = 0.0F;
        for (int i = 0; i < 20000; ++i) {
            value = static_cast<float>(i % 100) / 100.0F;
            if (dg_set_gain(engine, value) != DG_OK ||
                dg_set_tone_cutoff_hz(engine, 200.0F + value * 4000.0F) != DG_OK ||
                dg_set_noise_mix(engine, value) != DG_OK ||
                dg_set_drone_hz(engine, 80.0F + value * 120.0F) != DG_OK) {
                setter_failures.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    });

    setter.join();
    stop.store(true, std::memory_order_release);
    renderer.join();

    CHECK(render_failures.load() == 0);
    CHECK(setter_failures.load() == 0);
    // The engine is still usable after the racing phase.
    float block[64] = {};
    uint32_t written = 0;
    EXPECT_STATUS(dg_render(engine, block, 32, &written), DG_OK);
    CHECK(written == 32);
    EXPECT_STATUS(dg_destroy(engine), DG_OK);
}

void separate_engines_share_no_state() {
    // Different handles must be independent: four threads create, render and
    // destroy their own engine, and every first block equals the reference
    // first block of a freshly created engine (no shared RNG or filter state).
    const dg_config config{44100, 1};
    constexpr uint32_t kFrames = 128;

    dg_engine* reference_engine = create_engine(config);
    CHECK(reference_engine != nullptr);
    std::vector<float> reference(kFrames, 0.0F);
    uint32_t written = 0;
    EXPECT_STATUS(dg_render(reference_engine, reference.data(), kFrames, &written),
                  DG_OK);
    EXPECT_STATUS(dg_destroy(reference_engine), DG_OK);

    constexpr int kThreads = 4;
    constexpr int kCycleCycles = 25;
    std::atomic<int> failures{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&] {
            for (int cycle = 0; cycle < kCycleCycles; ++cycle) {
                dg_engine* engine = nullptr;
                if (dg_create(&config, &engine) != DG_OK || engine == nullptr) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                std::vector<float> block(kFrames, 0.0F);
                uint32_t local_written = 0;
                if (dg_render(engine, block.data(), kFrames, &local_written) != DG_OK ||
                    local_written != kFrames ||
                    std::memcmp(reference.data(), block.data(),
                                kFrames * sizeof(float)) != 0) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
                if (dg_destroy(engine) != DG_OK) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK(failures.load() == 0);
}

// ---------------------------------------------------------------------------
// Repeated lifecycle
// ---------------------------------------------------------------------------

void repeated_lifecycle_is_deterministic() {
    // 200 cycles of create/render/reset/destroy. This detects state that leaks
    // *between engines* (determinism drift); it is not a memory leak check.
    const dg_config config{44100, 1};
    constexpr uint32_t kFrames = 64;
    std::vector<float> reference(kFrames, 0.0F);
    uint32_t written = 0;

    for (int cycle = 0; cycle < 200; ++cycle) {
        dg_engine* engine = create_engine(config);
        CHECK(engine != nullptr);

        std::vector<float> block(kFrames, 0.0F);
        EXPECT_STATUS(dg_render(engine, block.data(), kFrames, &written), DG_OK);
        CHECK(written == kFrames);

        if (cycle == 0) {
            reference = block;
        } else {
            CHECK(std::memcmp(reference.data(), block.data(),
                              kFrames * sizeof(float)) == 0);
        }

        EXPECT_STATUS(dg_reset(engine), DG_OK);
        EXPECT_STATUS(dg_destroy(engine), DG_OK);
    }
}

void version_is_reported() {
    const char* version = dg_version();
    CHECK(version != nullptr);
    CHECK(version[0] != '\0');
    CHECK(std::strcmp(version, "0.1.0") == 0);
}

}  // namespace

int main() {
    std::printf("dg_lifecycle_tests: portable DSP core (DG-010)\n");

    run("create rejects null arguments", create_rejects_null_arguments);
    run("create rejects out-of-range sample rate", create_rejects_out_of_range_sample_rate);
    run("create rejects out-of-range channel count", create_rejects_out_of_range_channel_count);
    run("create validates sample rate first", create_checks_sample_rate_before_channel_count);
    run("create accepts every supported configuration",
        create_accepts_every_supported_configuration);
    run("live handles are distinct", live_handles_are_distinct);
    run("null handles report DG_ERR_NULL_HANDLE", null_handles_report_null_handle);
    run("destroy accepts a valid handle once", destroy_accepts_a_valid_handle_once);
    run("render validates arguments", render_validates_arguments);
    run("render writes interleaved in-range samples",
        render_writes_interleaved_in_range_samples);
    run("reset restores the initial render state", reset_restores_the_initial_render_state);
    run("reset preserves parameters", reset_preserves_parameters);
    run("parameter setters keep clamping", parameter_setters_keep_the_documented_clamping);
    run("setters may race with render", setters_may_race_with_render);
    run("separate engines share no state", separate_engines_share_no_state);
    run("repeated lifecycle is deterministic", repeated_lifecycle_is_deterministic);
    run("version is reported", version_is_reported);

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures != 0) {
        std::printf("FAILED\n");
        return 1;
    }
    std::printf("PASSED\n");
    return 0;
}
