#include "dsp_core.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace dreamguard {
namespace {

constexpr double kMinCutoffHz = 80.0;
constexpr double kMaxCutoffHz = 18000.0;
constexpr float kMinGain = 0.0F;
constexpr float kMaxGain = 1.0F;
constexpr float kMinDroneHz = 40.0F;
constexpr float kMaxDroneHz = 220.0F;
constexpr double kFadeInSeconds = 2.0;

double sanitize(double value, double fallback, double low, double high) {
    if (!std::isfinite(value)) {
        return fallback;
    }
    return std::clamp(value, low, high);
}

}  // namespace

void BiquadLowPass::configure(double sampleRate, double cutoffHz, double q) {
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0) {
        return;
    }
    const double nyquist = sampleRate * 0.5;
    const double cutoff = std::clamp(cutoffHz, kMinCutoffHz, nyquist * 0.9);
    const double omega = 2.0 * std::numbers::pi * cutoff / sampleRate;
    const double cosOmega = std::cos(omega);
    const double alpha = std::sin(omega) / (2.0 * std::max(q, 0.0001));

    const double a0 = 1.0 + alpha;
    b0_ = ((1.0 - cosOmega) * 0.5) / a0;
    b1_ = (1.0 - cosOmega) / a0;
    b2_ = b0_;
    a1_ = (-2.0 * cosOmega) / a0;
    a2_ = (1.0 - alpha) / a0;
}

float BiquadLowPass::process(float input) noexcept {
    const double x = static_cast<double>(input);
    const double y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    // Denormals in the recursive state can cost a lot of CPU on ARM.
    if (std::abs(z1_) < 1.0e-20) {
        z1_ = 0.0;
    }
    if (std::abs(z2_) < 1.0e-20) {
        z2_ = 0.0;
    }
    return static_cast<float>(y);
}

void BiquadLowPass::reset() noexcept {
    z1_ = 0.0;
    z2_ = 0.0;
}

PinkNoise::PinkNoise(std::uint32_t seed) noexcept : state_(seed) {
    reset(seed);
}

void PinkNoise::reset(std::uint32_t seed) noexcept {
    state_ = seed == 0 ? 0x9E3779B9u : seed;
    b0_ = b1_ = b2_ = b3_ = b4_ = b5_ = b6_ = 0.0;
}

float PinkNoise::white() noexcept {
    // xorshift32: fast, allocation-free, good enough for noise shaping.
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 17U;
    state_ ^= state_ << 5U;
    const float normalized = static_cast<float>(state_ >> 8U) / 8388608.0F;  // [0, 2)
    return normalized - 1.0F;
}

float PinkNoise::next() noexcept {
    const float w = white();
    b0_ = 0.99886 * b0_ + static_cast<double>(w) * 0.0555179;
    b1_ = 0.99332 * b1_ + static_cast<double>(w) * 0.0750759;
    b2_ = 0.96900 * b2_ + static_cast<double>(w) * 0.1538520;
    b3_ = 0.86650 * b3_ + static_cast<double>(w) * 0.3104856;
    b4_ = 0.55000 * b4_ + static_cast<double>(w) * 0.5329522;
    b5_ = -0.7616 * b5_ - static_cast<double>(w) * 0.0168980;
    const double pink = b0_ + b1_ + b2_ + b3_ + b4_ + b5_ + b6_ +
                        static_cast<double>(w) * 0.5362;
    b6_ = static_cast<double>(w) * 0.115926;
    return std::clamp(static_cast<float>(pink * 0.11), -1.0F, 1.0F);
}

SoundscapeRenderer::SoundscapeRenderer(double sampleRate, int channels) noexcept
    : sampleRate_(sampleRate > 0.0 ? sampleRate : 48000.0),
      channels_(std::clamp(channels, 1, 2)),
      noise_(0x51ED270Bu) {
    tone_.configure(sampleRate_, 1200.0);
    reset();
}

void SoundscapeRenderer::reset() noexcept {
    tone_.reset();
    noise_.reset(0x51ED270Bu);
    phaseA_ = 0.0;
    phaseB_ = 0.0;
    phaseLfo_ = 0.0;
    fade_ = 0.0;
}

void SoundscapeRenderer::setGain(float gain) noexcept {
    gain_.store(std::isfinite(gain) ? std::clamp(gain, kMinGain, kMaxGain) : 0.7F,
                std::memory_order_relaxed);
}

void SoundscapeRenderer::setToneCutoff(float cutoffHz) noexcept {
    const auto cutoff = static_cast<float>(
        sanitize(static_cast<double>(cutoffHz), 1200.0, kMinCutoffHz, kMaxCutoffHz));
    cutoffHz_.store(cutoff, std::memory_order_relaxed);
}

void SoundscapeRenderer::setNoiseMix(float mix) noexcept {
    noiseMix_.store(std::isfinite(mix) ? std::clamp(mix, 0.0F, 1.0F) : 0.35F,
                    std::memory_order_relaxed);
}

void SoundscapeRenderer::setDroneHz(float hz) noexcept {
    const auto frequency = static_cast<float>(
        sanitize(static_cast<double>(hz), 110.0, kMinDroneHz, kMaxDroneHz));
    droneHz_.store(frequency, std::memory_order_relaxed);
}

float SoundscapeRenderer::softLimit(float sample) noexcept {
    // tanh keeps transients musical while guaranteeing |output| < 1.
    return std::tanh(sample * 1.1F) / std::tanh(1.1F);
}

float SoundscapeRenderer::renderMonoFrame() noexcept {
    const double twoPi = 2.0 * std::numbers::pi;
    const double drone = static_cast<double>(droneHz_.load(std::memory_order_relaxed));

    phaseA_ += twoPi * drone / sampleRate_;
    phaseB_ += twoPi * (drone * 1.5) / sampleRate_;
    phaseLfo_ += twoPi * 0.05 / sampleRate_;  // one breath every 20 s
    if (phaseA_ >= twoPi) phaseA_ -= twoPi;
    if (phaseB_ >= twoPi) phaseB_ -= twoPi;
    if (phaseLfo_ >= twoPi) phaseLfo_ -= twoPi;

    const double lfo = 0.82 + 0.18 * std::sin(phaseLfo_);
    const double tonal = 0.62 * std::sin(phaseA_) + 0.38 * std::sin(phaseB_);
    const double noise = noise_.next();
    const double mix = static_cast<double>(noiseMix_.load(std::memory_order_relaxed));
    double sample = lfo * ((1.0 - mix) * tonal + mix * noise);

    sample = static_cast<double>(tone_.process(static_cast<float>(sample)));

    fade_ = std::min(1.0, fade_ + 1.0 / (kFadeInSeconds * sampleRate_));
    sample *= fade_ * static_cast<double>(gain_.load(std::memory_order_relaxed));

    return softLimit(static_cast<float>(sample * 0.85));
}

int SoundscapeRenderer::render(std::span<float> interleaved) noexcept {
    if (channels_ <= 0 || interleaved.empty()) {
        return 0;
    }
    const auto cutoff = static_cast<double>(
        cutoffHz_.load(std::memory_order_relaxed));
    if (std::abs(cutoff - appliedCutoffHz_) > 0.5) {
        tone_.configure(sampleRate_, cutoff);
        appliedCutoffHz_ = cutoff;
    }
    const auto frames = static_cast<int>(interleaved.size() /
                                         static_cast<std::size_t>(channels_));
    for (int frame = 0; frame < frames; ++frame) {
        const float mono = renderMonoFrame();
        for (int channel = 0; channel < channels_; ++channel) {
            interleaved[static_cast<std::size_t>(frame * channels_ + channel)] = mono;
        }
    }
    return frames;
}

}  // namespace dreamguard
