#pragma once

#include <atomic>
#include <cstdint>
#include <span>

namespace dreamguard {

inline constexpr char kDspVersion[] = "0.1.0";

/// RBJ-cookbook biquad low-pass filter. Coefficients are recalculated only when
/// the sample rate or cutoff changes, so rendering stays allocation-free.
class BiquadLowPass {
public:
    void configure(double sampleRate, double cutoffHz, double q = 0.7071067811865476);
    [[nodiscard]] float process(float input) noexcept;
    void reset() noexcept;

private:
    double b0_{1.0};
    double b1_{0.0};
    double b2_{0.0};
    double a1_{0.0};
    double a2_{0.0};
    double z1_{0.0};
    double z2_{0.0};
};

/// Cheap deterministic pink-noise generator (Paul Kellet's refined method).
/// Deterministic output keeps native behaviour reproducible across launches.
class PinkNoise {
public:
    explicit PinkNoise(std::uint32_t seed = 0x9E3779B9u) noexcept;
    void reset(std::uint32_t seed) noexcept;
    [[nodiscard]] float next() noexcept;

private:
    [[nodiscard]] float white() noexcept;

    std::uint32_t state_;
    double b0_{0.0};
    double b1_{0.0};
    double b2_{0.0};
    double b3_{0.0};
    double b4_{0.0};
    double b5_{0.0};
    double b6_{0.0};
};

/// Real-time safe soundscape: a slowly breathing dual-sine drone, pink noise,
/// a low-pass tone control, and a soft limiter. No allocation happens in render.
class SoundscapeRenderer {
public:
    SoundscapeRenderer(double sampleRate, int channels) noexcept;

    void reset() noexcept;

    void setGain(float gain) noexcept;
    void setToneCutoff(float cutoffHz) noexcept;
    void setNoiseMix(float mix) noexcept;
    void setDroneHz(float hz) noexcept;

    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] int channels() const noexcept { return channels_; }

    /// Renders interleaved float PCM and returns the number of frames written.
    int render(std::span<float> interleaved) noexcept;

private:
    [[nodiscard]] float renderMonoFrame() noexcept;
    [[nodiscard]] static float softLimit(float sample) noexcept;

    double sampleRate_;
    int channels_;

    BiquadLowPass tone_;
    PinkNoise noise_;

    double phaseA_{0.0};
    double phaseB_{0.0};
    double phaseLfo_{0.0};
    double fade_{0.0};
    double appliedCutoffHz_{1200.0};

    // Written by the UI thread, read by the audio thread. The filter is
    // reconfigured only on the audio thread at block boundaries.
    std::atomic<float> droneHz_{110.0F};
    std::atomic<float> cutoffHz_{1200.0F};
    std::atomic<float> gain_{0.7F};
    std::atomic<float> noiseMix_{0.35F};
};

}  // namespace dreamguard
