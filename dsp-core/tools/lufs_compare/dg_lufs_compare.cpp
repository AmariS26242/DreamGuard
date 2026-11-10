/* DG-012 LUFS reference-comparison harness.
 *
 * This program does three things, and nothing else:
 *
 *   1. It builds a fixed, deterministic set of synthetic PCM signals (mono and
 *      stereo, several levels, several sample rates, silence, a level step that
 *      exercises both gates, broadband noise, and a sub-400 ms stream) and
 *      synthesises them sample by sample. No RNG seeding from the clock, no
 *      floating-point fast-math, no resampling: the same build always writes the
 *      same bytes.
 *   2. It measures every signal with this workspace's dg_lufs analyser and
 *      prints / writes the readings together with the analytic expectation where
 *      one exists (steady sine tones only).
 *   3. With --wav-dir it writes every signal as a 32-bit IEEE-float WAV file so
 *      an external reference meter can be run over exactly the same bytes. With
 *      --libebur128 (only when this binary was built with
 *      -DDREAMGUARD_LUFS_COMPARE_EBUR128=ON) it also measures the same signals
 *      with libebur128 in-process and prints the deviations.
 *
 * What this program does NOT do: it does not invoke ffmpeg by itself. Driving
 * ffmpeg's `ebur128` filter is done by compare_with_ffmpeg.ps1 next to this
 * file, which reads the CSV this program writes. Keeping the process boundary
 * explicit means a missing ffmpeg can never be mistaken for a passing
 * comparison.
 *
 * The self-check (--selfcheck, registered as the CTest "dg_lufs_compare")
 * asserts only properties of this harness and of the analyser's internal
 * consistency:
 *   - whole-buffer and arbitrarily chunked processing are bit-identical;
 *   - a steady sine reads the analytic K-weighted value at its frequency;
 *   - mono reads exactly 10*log10(2) LU below stereo for the same generator;
 *   - silence and a 300 ms stream report integrated_ready == 0;
 *   - the level-step signal produces the block/gate counts the unit test pins
 *     (blocks == 67, blocks_used == 40);
 *   - the WAV container header and size are exactly as documented.
 * It is NOT evidence that the analyser agrees with a reference meter. Only
 * compare_with_ffmpeg.ps1 (or --libebur128) can produce that evidence, and the
 * acceptance tolerance for it is 0.1 LU (EBU Tech 3341 meter tolerance), set by
 * the caller and never widened to make a run pass.
 */

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if defined(DREAMGUARD_HAVE_LIBEBUR128)
extern "C" {
#include <ebur128.h>
}
#endif

#include "dreamguard/lufs.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinusInfinity = -std::numeric_limits<double>::infinity();

/* Absolute level of the quiet section of the level step. It must match the
 * -50 dBFS sections of tests/test_lufs.cpp: above the -70 LUFS absolute gate, but
 * more than 10 LU below the loud section so the relative gate drops it. */
constexpr double kLevelStepQuietDbfs = -50.0;

/* Declared up here because generate() needs it for the level-step signal. */
double dbfs_to_linear(double dbfs) { return std::pow(10.0, dbfs / 20.0); }

/* ------------------------------------------------------------------ */
/* Signals                                                             */
/* ------------------------------------------------------------------ */

enum class Signal { kSine, kSilence, kNoise, kLevelStep };

struct Case {
    std::string name;
    int32_t sample_rate = 48000;
    int32_t channels = 2;
    double seconds = 4.0;
    Signal signal = Signal::kSine;
    double frequency = 1000.0;  /* Hz, kSine only */
    double peak = 0.1;          /* linear amplitude of the left / mono channel */
    double right_peak = 0.1;    /* linear amplitude of the right channel */
    bool expect_integrated = true;
    std::string note;
};

const char* signal_name(Signal signal) {
    switch (signal) {
        case Signal::kSine:
            return "sine";
        case Signal::kSilence:
            return "silence";
        case Signal::kNoise:
            return "noise";
        case Signal::kLevelStep:
            return "levelstep";
    }
    return "unknown";
}

std::size_t frame_count_of(const Case& c) {
    const double frames = std::llround(c.seconds * static_cast<double>(c.sample_rate));
    return frames < 0.0 ? 0U : static_cast<std::size_t>(frames);
}

/* Deterministic xorshift32, mapped to [-1, 1). The state depends only on the
 * channel index, so every run and every machine produces the same samples. */
double uniform_noise(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return static_cast<double>(x >> 8) * (1.0 / 8388608.0) - 1.0;
}

void fill_sine(std::vector<float>* out, std::size_t begin_frame, std::size_t frames,
               const Case& c, double amplitude_scale) {
    const std::size_t channels = static_cast<std::size_t>(c.channels);
    const double rate = static_cast<double>(c.sample_rate);
    const double left = c.peak * amplitude_scale;
    const double right = c.right_peak * amplitude_scale;
    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t frame = begin_frame + i;
        const double phase = 2.0 * kPi * c.frequency * static_cast<double>(frame) / rate;
        const double value = std::sin(phase);
        out->at(frame * channels) = static_cast<float>(left * value);
        if (channels == 2) {
            out->at(frame * channels + 1) = static_cast<float>(right * value);
        }
    }
}

void fill_noise(std::vector<float>* out, std::size_t begin_frame, std::size_t frames,
                const Case& c) {
    const std::size_t channels = static_cast<std::size_t>(c.channels);
    uint32_t left_state = 0x9E3779B9U;
    uint32_t right_state = 0x85EBCA6BU;
    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t frame = begin_frame + i;
        out->at(frame * channels) =
            static_cast<float>(c.peak * uniform_noise(&left_state));
        if (channels == 2) {
            out->at(frame * channels + 1) =
                static_cast<float>(c.right_peak * uniform_noise(&right_state));
        }
    }
}

std::vector<float> generate(const Case& c) {
    const std::size_t channels = static_cast<std::size_t>(c.channels);
    const std::size_t frames = frame_count_of(c);
    std::vector<float> samples(frames * channels, 0.0F);
    switch (c.signal) {
        case Signal::kSine:
            fill_sine(&samples, 0, frames, c, 1.0);
            break;
        case Signal::kSilence:
            break;
        case Signal::kNoise:
            fill_noise(&samples, 0, frames, c);
            break;
        case Signal::kLevelStep: {
            /* Four sevenths at the case level, one seventh silence, two sevenths
             * at an ABSOLUTE -50 dBFS - the same construction tests/test_lufs.cpp
             * uses in test_gating(). The quiet section is specified in dBFS, not
             * relative to the loud section: fill_sine multiplies by c.peak, so
             * passing 10^(-50/20) as a scale would land at -70 dBFS (loud -50 dB),
             * which is below the -70 LUFS absolute gate and silently disappears
             * from blocks_after_absolute_gate. With the documented 7 s case this
             * is exactly 4 s / 1 s / 2 s at every supported rate (all supported
             * rates are multiples of 10). */
            const std::size_t loud = frames * 4U / 7U;
            const std::size_t quiet_begin = loud + frames / 7U;
            fill_sine(&samples, 0, loud, c, 1.0);
            Case quiet = c;
            quiet.peak = dbfs_to_linear(kLevelStepQuietDbfs);
            quiet.right_peak = dbfs_to_linear(kLevelStepQuietDbfs);
            fill_sine(&samples, quiet_begin, frames - quiet_begin, quiet, 1.0);
            break;
        }
    }
    return samples;
}

/* ------------------------------------------------------------------ */
/* Analytic expectation for a steady sine tone                         */
/* ------------------------------------------------------------------ */

double biquad_gain_db(const dg_biquad_coefficients& c, double rate, double frequency) {
    const double omega = 2.0 * kPi * frequency / rate;
    const std::complex<double> z1 = std::polar(1.0, -omega);
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> numerator = c.b0 + c.b1 * z1 + c.b2 * z2;
    const std::complex<double> denominator = 1.0 + c.a1 * z1 + c.a2 * z2;
    return 20.0 * std::log10(std::abs(numerator / denominator));
}

/* Loudness a steady sine of the given peak amplitude MUST read, by the
 * definition this component implements: -0.691 + 10*log10(sum_i G_i z_i) with
 * G = 1, so a sine reads the K-weighted power of its RMS level. Equal left and
 * right amplitudes are required; anything else is not analytic here and the
 * caller reports NaN. */
double analytic_lufs(const Case& c) {
    if (c.signal != Signal::kSine) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (c.channels == 2 && c.right_peak != c.peak) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double peak = c.peak;
    if (!(peak > 0.0)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    dg_lufs_k_weighting weighting{};
    if (dg_lufs_k_weighting_for_rate(c.sample_rate, &weighting) != DG_OK) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double gain_db = biquad_gain_db(weighting.shelf, static_cast<double>(c.sample_rate),
                                          c.frequency) +
                           biquad_gain_db(weighting.high_pass,
                                          static_cast<double>(c.sample_rate), c.frequency);
    const double channel_offset = c.channels == 1 ? 10.0 * std::log10(2.0) : 0.0;
    return 20.0 * std::log10(peak) + gain_db + DG_LUFS_LOUDNESS_OFFSET - channel_offset;
}

/* ------------------------------------------------------------------ */
/* Measurement                                                         */
/* ------------------------------------------------------------------ */

struct Result {
    dg_lufs_values values{};
    bool ok = false;
};

Result measure(const std::vector<float>& samples, int32_t rate, int32_t channels,
               bool chunked) {
    Result result;
    dg_lufs* lufs = nullptr;
    dg_lufs_config config{};
    config.sample_rate = rate;
    config.channel_count = channels;
    if (dg_lufs_create(&config, &lufs) != DG_OK || lufs == nullptr) {
        return result;
    }
    const std::size_t frames =
        channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;

    if (!chunked) {
        dg_pcm_view view{};
        view.samples = samples.empty() ? nullptr : samples.data();
        view.frame_count = static_cast<uint32_t>(frames);
        view.channel_count = channels;
        result.ok = dg_lufs_process(lufs, view) == DG_OK;
    } else {
        /* Split points deliberately straddle hop (rate/10) and block
         * (rate*4/10) boundaries, and include single-frame blocks. */
        const std::size_t chunk_sizes[] = {1U, 13U, 997U, 4800U, 4799U, 19200U, 7U, 65536U};
        std::size_t offset = 0;
        std::size_t index = 0;
        result.ok = true;
        while (offset < frames && result.ok) {
            std::size_t take = chunk_sizes[index % 8U];
            if (take > frames - offset) {
                take = frames - offset;
            }
            dg_pcm_view view{};
            view.samples =
                samples.data() + offset * static_cast<std::size_t>(channels);
            view.frame_count = static_cast<uint32_t>(take);
            view.channel_count = channels;
            result.ok = dg_lufs_process(lufs, view) == DG_OK;
            offset += take;
            ++index;
        }
    }
    if (result.ok) {
        result.ok = dg_lufs_snapshot(lufs, &result.values) == DG_OK;
    }
    dg_lufs_destroy(lufs);
    return result;
}

/* ------------------------------------------------------------------ */
/* WAV writing (32-bit IEEE float, uncompressed)                        */
/* ------------------------------------------------------------------ */

void put_u16(std::vector<uint8_t>* out, uint16_t value) {
    out->push_back(static_cast<uint8_t>(value & 0xFFU));
    out->push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
}

void put_u32(std::vector<uint8_t>* out, uint32_t value) {
    out->push_back(static_cast<uint8_t>(value & 0xFFU));
    out->push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
    out->push_back(static_cast<uint8_t>((value >> 16) & 0xFFU));
    out->push_back(static_cast<uint8_t>((value >> 24) & 0xFFU));
}

void put_tag(std::vector<uint8_t>* out, const char* tag) {
    for (int i = 0; i < 4; ++i) {
        out->push_back(static_cast<uint8_t>(tag[i]));
    }
}

std::vector<uint8_t> wav_bytes(const std::vector<float>& samples, int32_t channels,
                               int32_t rate, bool* ok) {
    const uint64_t data_bytes = static_cast<uint64_t>(samples.size()) * 4U;
    *ok = channels > 0 && rate > 0 && data_bytes + 36U <= 0xFFFFFFFFULL;
    if (!*ok) {
        return {};
    }
    std::vector<uint8_t> out;
    out.reserve(static_cast<std::size_t>(data_bytes) + 44U);
    put_tag(&out, "RIFF");
    put_u32(&out, static_cast<uint32_t>(36U + data_bytes));
    put_tag(&out, "WAVE");
    put_tag(&out, "fmt ");
    put_u32(&out, 16U);                                   /* PCM chunk size */
    put_u16(&out, static_cast<uint16_t>(3));              /* WAVE_FORMAT_IEEE_FLOAT */
    put_u16(&out, static_cast<uint16_t>(channels));
    put_u32(&out, static_cast<uint32_t>(rate));
    put_u32(&out, static_cast<uint32_t>(rate) *
                      static_cast<uint32_t>(channels) * 4U); /* byte rate */
    put_u16(&out, static_cast<uint16_t>(channels * 4));      /* block align */
    put_u16(&out, static_cast<uint16_t>(32));                /* bits per sample */
    put_tag(&out, "data");
    put_u32(&out, static_cast<uint32_t>(data_bytes));
    for (float sample : samples) {
        uint32_t bits = 0;
        std::memcpy(&bits, &sample, sizeof(bits));
        put_u32(&out, bits);
    }
    return out;
}

bool write_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes,
                std::string* error) {
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) {
        *error = "cannot open " + path.string();
        return false;
    }
    const std::size_t written =
        bytes.empty() ? 0U : std::fwrite(bytes.data(), 1U, bytes.size(), file);
    const bool closed_ok = std::fclose(file) == 0;
    if (written != bytes.size() || !closed_ok) {
        *error = "short write to " + path.string();
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Formatting                                                          */
/* ------------------------------------------------------------------ */

std::string format_db(double value, int precision = 3) {
    if (std::isnan(value)) {
        return "n/a";
    }
    if (!std::isfinite(value)) {
        return value < 0.0 ? "-inf" : "inf";
    }
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return buffer;
}

std::string csv_field(const std::string& text) {
    if (text.find_first_of(",\"\n") == std::string::npos) {
        return text;
    }
    std::string quoted = "\"";
    for (char ch : text) {
        if (ch == '"') {
            quoted += '"';
        }
        quoted += ch;
    }
    quoted += '"';
    return quoted;
}

/* ------------------------------------------------------------------ */
/* The case table                                                      */
/* ------------------------------------------------------------------ */

Case make_sine_case(const char* name, int32_t rate, int32_t channels, double seconds,
                    double frequency, double peak_dbfs, double right_peak_dbfs,
                    std::string note) {
    Case c;
    c.name = name;
    c.sample_rate = rate;
    c.channels = channels;
    c.seconds = seconds;
    c.signal = Signal::kSine;
    c.frequency = frequency;
    c.peak = dbfs_to_linear(peak_dbfs);
    c.right_peak = channels == 2 ? dbfs_to_linear(right_peak_dbfs) : 0.0;
    c.expect_integrated = true;
    c.note = std::move(note);
    return c;
}

std::vector<Case> make_cases() {
    std::vector<Case> cases;

    /* Calibration points: EBU Tech 3341 says a 1 kHz sine reads its peak level
     * in dBFS; this implementation is documented to land 0.024 LU below that. */
    cases.push_back(make_sine_case("stereo_1k_-20dB_48k", 48000, 2, 4.0, 1000.0, -20.0,
                                   -20.0, "Tech 3341 calibration, stereo"));
    cases.push_back(make_sine_case("mono_1k_-20dB_48k", 48000, 1, 4.0, 1000.0, -20.0,
                                   0.0, "same generator mono: 3.0103 LU lower"));
    cases.push_back(make_sine_case("stereo_1k_-23dB_48k", 48000, 2, 4.0, 1000.0, -23.0,
                                   -23.0, "Tech 3341 nominal level"));
    cases.push_back(make_sine_case("stereo_1k_-6dB_48k", 48000, 2, 4.0, 1000.0, -6.0,
                                   -6.0, "hot signal, still below clipping"));
    cases.push_back(make_sine_case("stereo_1k_-60dB_48k", 48000, 2, 4.0, 1000.0, -60.0,
                                   -60.0, "just above the -70 LUFS absolute gate"));

    /* K-weighting shape: the high-pass corner and the high shelf. */
    cases.push_back(make_sine_case("stereo_100Hz_-20dB_48k", 48000, 2, 4.0, 100.0,
                                   -20.0, -20.0, "high-pass region"));
    cases.push_back(make_sine_case("stereo_10kHz_-20dB_48k", 48000, 2, 4.0, 10000.0,
                                   -20.0, -20.0, "high-shelf region"));
    cases.push_back(make_sine_case("stereo_asym_L-6_R-12dB_48k", 48000, 2, 4.0, 1000.0,
                                   -6.0, -12.0, "asymmetric stereo, no analytic value"));

    /* Derived (non-published) coefficient sets. */
    cases.push_back(make_sine_case("stereo_1k_-20dB_44k1", 44100, 2, 4.0, 1000.0, -20.0,
                                   -20.0, "bilinear-derived coefficients at 44.1 kHz"));
    cases.push_back(make_sine_case("stereo_1k_-20dB_96k", 96000, 2, 4.0, 1000.0, -20.0,
                                   -20.0, "bilinear-derived coefficients at 96 kHz"));
    cases.push_back(make_sine_case("stereo_1k_-20dB_192k", 192000, 2, 4.0, 1000.0,
                                   -20.0, -20.0, "top of the supported rate range"));
    cases.push_back(make_sine_case("mono_1k_-20dB_8k", 8000, 1, 4.0, 1000.0, -20.0, 0.0,
                                   "bottom of the supported rate range"));

    /* Broadband noise: exercises the whole K-weighting curve at once. */
    {
        Case c;
        c.name = "stereo_noise_-20dB_48k";
        c.sample_rate = 48000;
        c.channels = 2;
        c.seconds = 5.0;
        c.signal = Signal::kNoise;
        c.peak = dbfs_to_linear(-20.0);
        c.right_peak = dbfs_to_linear(-20.0);
        c.expect_integrated = true;
        c.note = "deterministic xorshift noise, exercises the whole K curve";
        cases.push_back(c);
    }

    /* Both gates: loud, silence, then -50 dBFS (below loud - 10 LU). */
    {
        Case c;
        c.name = "mono_levelstep_4s_1s_2s_48k";
        c.sample_rate = 48000;
        c.channels = 1;
        c.seconds = 7.0;
        c.signal = Signal::kLevelStep;
        c.frequency = 1000.0;
        c.peak = dbfs_to_linear(-20.0);
        c.right_peak = 0.0;
        c.expect_integrated = true;
        c.note = "4 s -20 dBFS, 1 s silence, 2 s -50 dBFS: relative gate must drop the tail";
        cases.push_back(c);
    }

    /* No integrated value: below one 400 ms block. */
    {
        Case c;
        c.name = "stereo_silence_2s_48k";
        c.sample_rate = 48000;
        c.channels = 2;
        c.seconds = 2.0;
        c.signal = Signal::kSilence;
        c.peak = 0.0;
        c.right_peak = 0.0;
        c.expect_integrated = false;
        c.note = "digital silence: integrated_ready == 0, -inf";
        cases.push_back(c);
    }
    {
        Case c;
        c.name = "stereo_1k_-20dB_0p3s_48k";
        c.sample_rate = 48000;
        c.channels = 2;
        c.seconds = 0.3;
        c.signal = Signal::kSine;
        c.frequency = 1000.0;
        c.peak = dbfs_to_linear(-20.0);
        c.right_peak = dbfs_to_linear(-20.0);
        c.expect_integrated = false;
        c.note = "shorter than one 400 ms block";
        cases.push_back(c);
    }

    return cases;
}

/* ------------------------------------------------------------------ */
/* Self-check                                                          */
/* ------------------------------------------------------------------ */

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_failures;
    std::printf("  FAIL %s\n", what.c_str());
}

void check_near(double actual, double expected, double tolerance,
                const std::string& what) {
    ++g_checks;
    const bool both_finite = std::isfinite(actual) && std::isfinite(expected);
    const bool ok = both_finite ? std::fabs(actual - expected) <= tolerance
                                : (actual == expected);
    if (ok) {
        return;
    }
    ++g_failures;
    std::printf("  FAIL %s: actual %s, expected %s (+/- %g)\n", what.c_str(),
                format_db(actual).c_str(), format_db(expected).c_str(), tolerance);
}

bool same_values(const dg_lufs_values& a, const dg_lufs_values& b) {
    return a.momentary_lufs == b.momentary_lufs &&
           a.short_term_lufs == b.short_term_lufs &&
           a.integrated_lufs == b.integrated_lufs && a.blocks == b.blocks &&
           a.blocks_used == b.blocks_used &&
           a.blocks_after_absolute_gate == b.blocks_after_absolute_gate &&
           a.blocks_dropped == b.blocks_dropped &&
           a.pending_frames == b.pending_frames &&
           a.integrated_ready == b.integrated_ready;
}

int run_selfcheck() {
    std::printf("dg_lufs_compare self-check\n");
    const std::vector<Case> cases = make_cases();

    /* Chunking invariance for every case. */
    for (const Case& c : cases) {
        const std::vector<float> samples = generate(c);
        const Result whole = measure(samples, c.sample_rate, c.channels, false);
        const Result chunked = measure(samples, c.sample_rate, c.channels, true);
        check(whole.ok, c.name + ": whole-buffer measurement failed");
        check(chunked.ok, c.name + ": chunked measurement failed");
        if (whole.ok && chunked.ok) {
            check(same_values(whole.values, chunked.values),
                  c.name + ": chunked results differ from whole-buffer results");
        }
    }

    /* Analytic reading of steady sines. Every supported rate divides 100 ms into
     * an integer number of cycles at 100 Hz, 1 kHz and 10 kHz, so the analytic
     * value is exact apart from filter settling, which 4 s buries far below
     * 1e-3 LU. */
    for (const Case& c : cases) {
        /* Only streams that are long enough to hold a 400 ms block may assert a
         * number here; the sub-block cases are covered by the readiness check. */
        if (!c.expect_integrated) {
            continue;
        }
        const double expected = analytic_lufs(c);
        if (!std::isfinite(expected)) {
            continue;
        }
        const Result result = measure(generate(c), c.sample_rate, c.channels, false);
        if (!result.ok) {
            check(false, c.name + ": measurement failed");
            continue;
        }
        check(result.values.integrated_ready == 1,
              c.name + ": integrated must be available");
        check_near(result.values.integrated_lufs, expected, 1e-3,
                   c.name + ": integrated vs analytic K-weighted value");
        check_near(result.values.momentary_lufs, expected, 1e-3,
                   c.name + ": momentary vs analytic value");
        if (c.seconds >= 3.0) {
            check_near(result.values.short_term_lufs, expected, 1e-3,
                       c.name + ": short-term vs analytic value");
        }
    }

    /* mono versus stereo, same generator and same peak. */
    {
        Case stereo = make_sine_case("stereo_ref", 48000, 2, 4.0, 1000.0, -20.0, -20.0, "");
        Case mono = make_sine_case("mono_ref", 48000, 1, 4.0, 1000.0, -20.0, 0.0, "");
        const Result s = measure(generate(stereo), 48000, 2, false);
        const Result m = measure(generate(mono), 48000, 1, false);
        check(s.ok && m.ok, "mono/stereo reference measurement failed");
        if (s.ok && m.ok) {
            check_near(s.values.integrated_lufs - m.values.integrated_lufs,
                       10.0 * std::log10(2.0), 1e-9,
                       "stereo - mono must be exactly 10*log10(2)");
        }
    }

    /* Readiness for the short and silent streams. */
    for (const Case& c : cases) {
        if (c.expect_integrated) {
            continue;
        }
        const Result result = measure(generate(c), c.sample_rate, c.channels, false);
        check(result.ok, c.name + ": measurement failed");
        if (!result.ok) {
            continue;
        }
        check(result.values.integrated_ready == 0,
              c.name + ": integrated must not be ready");
        check(result.values.integrated_lufs == kMinusInfinity,
              c.name + ": integrated must be -inf");
    }

    /* Gate bookkeeping pinned by tests/test_lufs.cpp: 7 s -> 70 hops -> 67
     * overlapping 400 ms blocks; the -50 dBFS tail must fail the relative gate,
     * leaving the 37 fully loud blocks and the 3 loud->silence transitions. */
    for (const Case& c : cases) {
        if (c.signal != Signal::kLevelStep) {
            continue;
        }
        const Result result = measure(generate(c), c.sample_rate, c.channels, false);
        check(result.ok, c.name + ": measurement failed");
        if (!result.ok) {
            continue;
        }
        check(result.values.blocks == 67U, c.name + ": expected 67 blocks");
        check(result.values.blocks_after_absolute_gate >= 60U,
              c.name + ": expected >= 60 blocks above the absolute gate");
        check(result.values.blocks_used == 40U, c.name + ": expected 40 surviving blocks");
        const double expected =
            20.0 * std::log10(c.peak) - 10.0 * std::log10(2.0);
        check_near(result.values.integrated_lufs, expected, 0.5,
                   c.name + ": integrated vs block model");

        /* Momentary covers the last 400 ms, i.e. only the quiet tail, so it pins
         * the ABSOLUTE level of that section. This is the check that the -70 dBFS
         * signal bug could not fool: with the tail 20 dB too quiet the tail blocks
         * still failed the relative gate (blocks_used stayed 40) but dropped out of
         * blocks_after_absolute_gate entirely. */
        Case quiet_probe;
        quiet_probe.name = c.name + "_quiet_tail";
        quiet_probe.sample_rate = c.sample_rate;
        quiet_probe.channels = 1;
        quiet_probe.signal = Signal::kSine;
        quiet_probe.frequency = c.frequency;
        quiet_probe.peak = dbfs_to_linear(kLevelStepQuietDbfs);
        quiet_probe.right_peak = quiet_probe.peak;
        const double quiet_analytic = analytic_lufs(quiet_probe);
        check(std::isfinite(quiet_analytic),
              c.name + ": the quiet tail has no analytic value");
        check_near(result.values.momentary_lufs, quiet_analytic, 0.1,
                   c.name + ": momentary of the quiet tail");
    }

    /* WAV container: exact header fields and size. */
    {
        const Case c = make_sine_case("wav_ref", 48000, 2, 0.5, 1000.0, -20.0, -20.0, "");
        const std::vector<float> samples = generate(c);
        bool ok = false;
        const std::vector<uint8_t> bytes = wav_bytes(samples, c.channels, c.sample_rate, &ok);
        check(ok, "wav_ref: container rejected");
        check(bytes.size() == 44U + samples.size() * 4U, "wav_ref: unexpected file size");
        if (bytes.size() >= 44U) {
            check(std::memcmp(bytes.data(), "RIFF", 4) == 0, "wav_ref: missing RIFF tag");
            check(std::memcmp(bytes.data() + 8, "WAVE", 4) == 0, "wav_ref: missing WAVE tag");
            check(std::memcmp(bytes.data() + 36, "data", 4) == 0, "wav_ref: missing data tag");
            const uint32_t riff_size = static_cast<uint32_t>(bytes[4]) |
                                       (static_cast<uint32_t>(bytes[5]) << 8) |
                                       (static_cast<uint32_t>(bytes[6]) << 16) |
                                       (static_cast<uint32_t>(bytes[7]) << 24);
            check(riff_size == bytes.size() - 8U, "wav_ref: RIFF size mismatch");
            const uint16_t format = static_cast<uint16_t>(bytes[20] | (bytes[21] << 8));
            check(format == 3U, "wav_ref: format must be IEEE float (3)");
            const uint16_t bits = static_cast<uint16_t>(bytes[34] | (bytes[35] << 8));
            check(bits == 32U, "wav_ref: bits per sample must be 32");
        }
    }

    std::printf("  %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Normal run                                                          */
/* ------------------------------------------------------------------ */

void print_header() {
    std::printf("%-28s %7s %3s %8s %9s %9s %9s %9s %6s %5s %6s  %s\n", "case", "rate",
                "ch", "seconds", "momentary", "short", "integrated", "analytic",
                "blocks", "used", "ready", "note");
}

void print_row(const Case& c, const Result& result, double analytic) {
    std::printf("%-28s %7d %3d %8.2f %9s %9s %9s %9s %6llu %5llu %6s  %s\n",
                c.name.c_str(), static_cast<int>(c.sample_rate),
                static_cast<int>(c.channels), c.seconds,
                format_db(result.values.momentary_lufs).c_str(),
                format_db(result.values.short_term_lufs).c_str(),
                format_db(result.values.integrated_lufs).c_str(),
                format_db(analytic).c_str(),
                static_cast<unsigned long long>(result.values.blocks),
                static_cast<unsigned long long>(result.values.blocks_used),
                result.values.integrated_ready ? "yes" : "no", c.note.c_str());
}

int run_measurements(const std::filesystem::path& wav_dir,
                     const std::filesystem::path& csv_path, bool write_wavs) {
    const std::vector<Case> cases = make_cases();
    std::printf("dg_lufs_compare: %zu synthetic cases\n",
                static_cast<std::size_t>(cases.size()));
    print_header();

    std::vector<std::string> csv_rows;
    csv_rows.push_back(
        "case,sample_rate,channels,seconds,signal,frequency_hz,peak_dbfs,"
        "expect_integrated,momentary_lufs,short_term_lufs,integrated_lufs,"
        "blocks,blocks_after_absolute_gate,blocks_used,blocks_dropped,"
        "pending_frames,analytic_lufs,note,wav");

    int failures = 0;
    for (const Case& c : cases) {
        const std::vector<float> samples = generate(c);
        const Result result = measure(samples, c.sample_rate, c.channels, false);
        const double analytic = analytic_lufs(c);
        if (!result.ok) {
            std::printf("  FAILED to measure %s\n", c.name.c_str());
            ++failures;
            continue;
        }
        print_row(c, result, analytic);

        std::string wav_name;
        if (write_wavs) {
            bool container_ok = false;
            const std::vector<uint8_t> bytes =
                wav_bytes(samples, c.channels, c.sample_rate, &container_ok);
            if (!container_ok) {
                std::printf("  FAILED to encode %s\n", c.name.c_str());
                ++failures;
                continue;
            }
            const std::filesystem::path path = wav_dir / (c.name + ".wav");
            std::string error;
            if (!write_file(path, bytes, &error)) {
                std::printf("  FAILED to write %s: %s\n", path.string().c_str(),
                            error.c_str());
                ++failures;
                continue;
            }
            wav_name = path.filename().string();
        }

        char row[1024] = {};
        std::snprintf(
            row, sizeof(row),
            "%s,%d,%d,%.3f,%s,%.1f,%.4f,%d,%s,%s,%s,%llu,%llu,%llu,%llu,%u,%s,%s,%s",
            csv_field(c.name).c_str(), static_cast<int>(c.sample_rate),
            static_cast<int>(c.channels), c.seconds, signal_name(c.signal), c.frequency,
            20.0 * std::log10(c.peak > 0.0 ? c.peak : 1.0),
            c.expect_integrated ? 1 : 0,
            format_db(result.values.momentary_lufs, 6).c_str(),
            format_db(result.values.short_term_lufs, 6).c_str(),
            format_db(result.values.integrated_lufs, 6).c_str(),
            static_cast<unsigned long long>(result.values.blocks),
            static_cast<unsigned long long>(result.values.blocks_after_absolute_gate),
            static_cast<unsigned long long>(result.values.blocks_used),
            static_cast<unsigned long long>(result.values.blocks_dropped),
            static_cast<unsigned>(result.values.pending_frames),
            format_db(analytic, 6).c_str(), csv_field(c.note).c_str(),
            csv_field(wav_name).c_str());
        csv_rows.push_back(row);
    }

    if (!csv_path.empty()) {
        std::vector<uint8_t> bytes;
        for (const std::string& row : csv_rows) {
            bytes.insert(bytes.end(), row.begin(), row.end());
            bytes.push_back('\n');
        }
        std::string error;
        if (!write_file(csv_path, bytes, &error)) {
            std::printf("  FAILED to write CSV: %s\n", error.c_str());
            ++failures;
        } else {
            std::printf("\nwrote %s\n", csv_path.string().c_str());
        }
    }
    return failures == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Optional in-process libebur128 reference                            */
/* ------------------------------------------------------------------ */

#if defined(DREAMGUARD_HAVE_LIBEBUR128)
struct ReferenceReading {
    double integrated = kMinusInfinity;
    double momentary = kMinusInfinity;
    double short_term = kMinusInfinity;
    bool ok = false;
};

ReferenceReading libebur128_measure(const std::vector<float>& samples, int32_t rate,
                                    int32_t channels) {
    ReferenceReading reading;
    const std::size_t frames =
        channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
    ebur128_state* state = ebur128_init(static_cast<unsigned>(channels),
                                        static_cast<unsigned long>(rate),
                                        EBUR128_MODE_I | EBUR128_MODE_M | EBUR128_MODE_S);
    if (state == nullptr) {
        return reading;
    }
    if (ebur128_add_frames_float(state, samples.data(), frames) != EBUR128_SUCCESS) {
        ebur128_destroy(&state);
        return reading;
    }
    if (ebur128_loudness_global(state, &reading.integrated) != EBUR128_SUCCESS) {
        reading.integrated = kMinusInfinity;
    }
    if (ebur128_loudness_momentary(state, &reading.momentary) != EBUR128_SUCCESS) {
        reading.momentary = kMinusInfinity;
    }
    if (ebur128_loudness_shortterm(state, &reading.short_term) != EBUR128_SUCCESS) {
        reading.short_term = kMinusInfinity;
    }
    reading.ok = true;
    ebur128_destroy(&state);
    return reading;
}
#endif

int run_libebur128(double tolerance) {
#if !defined(DREAMGUARD_HAVE_LIBEBUR128)
    (void)tolerance;
    std::printf(
        "This binary was built without libebur128 support.\n"
        "Reconfigure with:\n"
        "  cmake -S dsp-core -B dsp-core/build/host -DDREAMGUARD_DSP_BUILD_TESTS=ON \\\n"
        "        -DDREAMGUARD_DSP_BUILD_TOOLS=ON -DDREAMGUARD_LUFS_COMPARE_EBUR128=ON\n"
        "and point CMake at the library with -DEBUR128_INCLUDE_DIR=<dir> and\n"
        "-DEBUR128_LIBRARY=<file> if it is not installed in a default location.\n");
    return 3;
#else
    std::printf("dg_lufs_compare: libebur128 reference, tolerance %.3f LU\n", tolerance);
    std::printf("%-28s %11s %11s %9s  %s\n", "case", "ours", "libebur128", "delta",
                "verdict");
    const std::vector<Case> cases = make_cases();
    int compared = 0;
    int passed = 0;
    int skipped = 0;
    int failed = 0;
    double worst = 0.0;
    std::string worst_case;
    for (const Case& c : cases) {
        const std::vector<float> samples = generate(c);
        const Result ours = measure(samples, c.sample_rate, c.channels, false);
        const ReferenceReading reference =
            libebur128_measure(samples, c.sample_rate, c.channels);
        if (!ours.ok || !reference.ok) {
            std::printf("%-28s %11s %11s %9s  ERROR\n", c.name.c_str(), "err", "err", "-");
            ++failed;
            continue;
        }
        if (!c.expect_integrated) {
            std::printf("%-28s %11s %11s %9s  n/a (no 400 ms block)\n", c.name.c_str(),
                        format_db(ours.values.integrated_lufs).c_str(),
                        format_db(reference.integrated).c_str(), "-");
            ++skipped;
            continue;
        }
        ++compared;
        const bool both_finite = std::isfinite(ours.values.integrated_lufs) &&
                                 std::isfinite(reference.integrated);
        const double delta = both_finite
                                 ? ours.values.integrated_lufs - reference.integrated
                                 : 0.0;
        const bool pass = both_finite && std::fabs(delta) <= tolerance;
        if (pass) {
            ++passed;
            if (std::fabs(delta) > std::fabs(worst)) {
                worst = delta;
                worst_case = c.name;
            }
        } else {
            ++failed;
        }
        std::printf("%-28s %11s %11s %9s  %s\n", c.name.c_str(),
                    format_db(ours.values.integrated_lufs).c_str(),
                    format_db(reference.integrated).c_str(), format_db(delta).c_str(),
                    pass ? "PASS" : "FAIL");
    }
    std::printf("\ncompared %d, passed %d, failed %d, skipped %d, worst |delta| %.3f LU",
                compared, passed, failed, skipped, std::fabs(worst));
    if (!worst_case.empty()) {
        std::printf(" (%s)", worst_case.c_str());
    }
    std::printf("\n");
    return failed == 0 && compared > 0 ? 0 : 1;
#endif
}

void print_usage() {
    std::printf(
        "usage: dg_lufs_compare [mode] [options]\n"
        "\n"
        "modes:\n"
        "  --selfcheck            run the harness self-check (CTest dg_lufs_compare)\n"
        "  --libebur128           compare against libebur128 in-process (needs the\n"
        "                         build option DREAMGUARD_LUFS_COMPARE_EBUR128=ON)\n"
        "  (default)              measure every case and print the table\n"
        "\n"
        "options:\n"
        "  --wav-dir <dir>        also write every case as a 32-bit float WAV\n"
        "  --csv <file>           write our readings as CSV (consumed by\n"
        "                         compare_with_ffmpeg.ps1)\n"
        "  --tolerance <lu>       tolerance for --libebur128 (default 0.1)\n"
        "  --help                 print this text\n");
}

}  // namespace

int main(int argc, char** argv) {
    bool selfcheck = false;
    bool compare_libebur128 = false;
    bool write_wavs = false;
    bool have_wav_dir = false;
    double tolerance = 0.1;
    std::filesystem::path wav_dir;
    std::filesystem::path csv_path;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            print_usage();
            return 0;
        } else if (argument == "--selfcheck") {
            selfcheck = true;
        } else if (argument == "--libebur128") {
            compare_libebur128 = true;
        } else if (argument == "--wav-dir" && i + 1 < argc) {
            wav_dir = argv[++i];
            write_wavs = true;
            have_wav_dir = true;
        } else if (argument == "--csv" && i + 1 < argc) {
            csv_path = argv[++i];
        } else if (argument == "--tolerance" && i + 1 < argc) {
            tolerance = std::atof(argv[++i]);
        } else {
            std::printf("unknown argument: %s\n", argument.c_str());
            print_usage();
            return 2;
        }
    }
    if (have_wav_dir) {
        std::error_code error;
        std::filesystem::create_directories(wav_dir, error);
        if (error) {
            std::printf("cannot create %s: %s\n", wav_dir.string().c_str(),
                        error.message().c_str());
            return 2;
        }
    }
    if (selfcheck) {
        return run_selfcheck();
    }
    if (compare_libebur128) {
        return run_libebur128(tolerance);
    }
    return run_measurements(wav_dir, csv_path, write_wavs);
}
