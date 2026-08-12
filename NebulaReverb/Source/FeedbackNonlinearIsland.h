#pragma once
// WoManus Platform v5 | PearlLeash | Copyright 2026 Tiffani LeBlanc
//
// FeedbackNonlinearIsland — shared feedback-path nonlinear processor (Phase A3).
//
// Processing order (explicit):
//   1) Apply bounded feedback gain
//   2) One-pole damping (HF absorption / runaway control)
//   3) Optional ADAA tanh saturation (ADAAWaveshaper<1>)
//   4) Soft gain compensation vs drive
//   5) SignalSanityGuard sanitize (NaN/Inf/denorm/clamp)
//
// Oversampling policy (do not duplicate factory 4×):
//   FeedbackOsMode::Off      — default; rely on product mandatory 4× OS shell
//   FeedbackOsMode::Nested2x — optional nested 2× around ADAA only when product
//                              sets fbOversample / nested mode; adds ~2 samples
//                              latency and extra CPU; off unless requested
//
// Zero allocation after prepare(). Deterministic reset().
// Not a CAPABILITY-REGISTRY chain id — generate.js feature flag only.

#include "ADAAWaveshaper.h"
#include "SignalSanityGuard.h"

#include <algorithm>
#include <array>
#include <cmath>

enum class FeedbackOsMode : int
{
    Off = 0,
    Nested2x = 1
};

class FeedbackNonlinearIsland
{
public:
    static constexpr float kMaxFeedback = 0.95f;
    static constexpr float kMinDrive = 0.5f;
    static constexpr float kMaxDrive = 3.5f;
    static constexpr float kDefaultDrive = 1.0f;
    static constexpr float kDefaultDamp = 0.35f;
    /** Approximate nested-2× group delay in samples (half-band FIR). */
    static constexpr int kNestedOsLatencySamples = 2;

    void prepare (double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        shaper_.prepare (sampleRate_);
        nestedShaper_.prepare (sampleRate_ * 2.0);
        reset();
    }

    void reset() noexcept
    {
        shaper_.reset();
        nestedShaper_.reset();
        dampState_ = 0.0f;
        upState_.fill (0.0f);
        downState_.fill (0.0f);
        prevIn_ = 0.0f;
    }

    double getSampleRate() const noexcept { return sampleRate_; }

    static float clampFeedback (float feedback) noexcept
    {
        return std::max (0.0f, std::min (kMaxFeedback, feedback));
    }

    static float clampDrive (float drive) noexcept
    {
        return std::max (kMinDrive, std::min (kMaxDrive, drive));
    }

    static float clampDamp (float damp) noexcept
    {
        return std::max (0.0f, std::min (1.0f, damp));
    }

    static FeedbackOsMode clampOsMode (int mode) noexcept
    {
        return mode >= 1 ? FeedbackOsMode::Nested2x : FeedbackOsMode::Off;
    }

    /** Latency contributed by this island alone (factory OS is separate). */
    static int latencySamplesFor (FeedbackOsMode mode) noexcept
    {
        return mode == FeedbackOsMode::Nested2x ? kNestedOsLatencySamples : 0;
    }

    /**
     * Map UI damp [0,1] → one-pole coefficient.
     * 0 = almost no damping (bright regen); 1 = heavy HF absorption.
     */
    static float dampCoefficient (float dampAmount) noexcept
    {
        const float d = clampDamp (dampAmount);
        // Perceptual-ish: more sensitive in the upper half.
        const float shaped = d * d;
        return 0.05f + 0.90f * shaped;
    }

    /**
     * Soft makeup so higher drive does not explode loop gain.
     * At drive=1 → ~1.0; at drive=3.5 → attenuated.
     */
    static float gainCompensation (float drive) noexcept
    {
        const float d = clampDrive (drive);
        return 1.0f / std::sqrt (std::max (1.0f, d));
    }

    /**
     * Process one feedback tap sample.
     * @param wetTap          delay/reverb tap before feedback gain
     * @param feedbackAmount  0..0.95
     * @param drive           ADAA drive (ignored if satEnable=false)
     * @param dampAmount      0..1 damping
     * @param satEnable       optional ADAA stage
     * @param osMode          Off (default) or Nested2x
     * @return sanitized feedback contribution to add to the dry write
     */
    float process (
        float wetTap,
        float feedbackAmount,
        float drive,
        float dampAmount,
        bool satEnable = true,
        FeedbackOsMode osMode = FeedbackOsMode::Off) noexcept
    {
        float x = wetTap * clampFeedback (feedbackAmount);

        // 2) Damping
        const float coeff = dampCoefficient (dampAmount);
        dampState_ += coeff * (x - dampState_);
        x = dampState_;

        // 3–4) Optional ADAA (+ nested OS policy) + gain compensation
        if (satEnable)
        {
            const float d = clampDrive (drive);
            if (osMode == FeedbackOsMode::Nested2x)
                x = processNested2xAdaa (x, d);
            else
                x = shaper_.processSample (x, d);
            x *= gainCompensation (d);
        }

        // 5) Sanity / runaway clamp
        return SignalSanityGuard::sanitize (x);
    }

private:
    double sampleRate_ = 44100.0;
    ADAAWaveshaper<1> shaper_ {};
    ADAAWaveshaper<1> nestedShaper_ {};
    float dampState_ = 0.0f;

    // Nested 2× half-band island (juce-free; mirrors Oversampling2x topology
    // but runs ADAA instead of a second tanh).
    std::array<float, 4> upState_ {};
    std::array<float, 4> downState_ {};
    float prevIn_ = 0.0f;

    static constexpr float h0 = 0.0625f;
    static constexpr float h2 = 0.5625f;

    static float halfband (std::array<float, 4>& z, float x) noexcept
    {
        const float y = h0 * (x + z[3]) + h2 * z[1];
        z[3] = z[2];
        z[2] = z[1];
        z[1] = z[0];
        z[0] = x;
        return y;
    }

    float processNested2xAdaa (float input, float drive) noexcept
    {
        const float mid = 0.5f * (prevIn_ + input);
        prevIn_ = input;
        const float y0 = nestedShaper_.processSample (halfband (upState_, mid), drive);
        const float y1 = nestedShaper_.processSample (halfband (upState_, input), drive);
        const float even = halfband (downState_, y0);
        (void) halfband (downState_, y1);
        return even;
    }
};

/** Stereo pair for independent L/R feedback paths. */
class StereoFeedbackNonlinearIsland
{
public:
    void prepare (double sampleRate) noexcept
    {
        left_.prepare (sampleRate);
        right_.prepare (sampleRate);
    }

    void reset() noexcept
    {
        left_.reset();
        right_.reset();
    }

    FeedbackNonlinearIsland& left() noexcept { return left_; }
    FeedbackNonlinearIsland& right() noexcept { return right_; }

    float processLeft (
        float wetTap,
        float feedbackAmount,
        float drive,
        float dampAmount,
        bool satEnable = true,
        int osMode = 0) noexcept
    {
        return left_.process (
            wetTap,
            feedbackAmount,
            drive,
            dampAmount,
            satEnable,
            FeedbackNonlinearIsland::clampOsMode (osMode));
    }

    float processRight (
        float wetTap,
        float feedbackAmount,
        float drive,
        float dampAmount,
        bool satEnable = true,
        int osMode = 0) noexcept
    {
        return right_.process (
            wetTap,
            feedbackAmount,
            drive,
            dampAmount,
            satEnable,
            FeedbackNonlinearIsland::clampOsMode (osMode));
    }

private:
    FeedbackNonlinearIsland left_;
    FeedbackNonlinearIsland right_;
};
