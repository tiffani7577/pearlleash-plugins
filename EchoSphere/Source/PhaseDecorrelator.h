#pragma once
// WoManus Platform v5 | PearlLeash | Copyright 2026 Tiffani LeBlanc
//
// PhaseDecorrelator — fixed-delay allpass stereo decorrelation with a
// gain-safe Mid/Side width contract (Phase A2).
//
// Contract:
//   width = 0 → exact dry passthrough (mono position)
//   width in (0,1] → dry mid preserved; side = blend(drySide, decorrSide) * width law
//   decorrelate in [0,1] → how hard the wet side is allpassed
//   constant-power side mix; no uncontrolled side boost; mid never inverted
// Zero allocation after prepare(). Deterministic reset().

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

class PhaseDecorrelator
{
public:
    static constexpr int kDelayL = 47;
    static constexpr int kDelayR = 73;
    static constexpr float kAllpassCoeff = 0.7f;

    void prepare (double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        reset();
    }

    void reset() noexcept
    {
        bufferL_.fill (0.0f);
        bufferR_.fill (0.0f);
        writeL_ = 0;
        writeR_ = 0;
    }

    double getSampleRate() const noexcept { return sampleRate_; }

    static float clampWidth (float width) noexcept { return clamp01 (width); }
    static float clampDecorrelate (float amount) noexcept { return clamp01 (amount); }

    /**
     * Gain-safe stereo width with dry mid preservation.
     * width=0 → left/right unchanged.
     * Mid is always taken from the dry input so mono collapse stays stable.
     */
    void process (float& left, float& right, float widthAmount, float decorrelateAmount = 1.0f) noexcept
    {
        const float dryL = left;
        const float dryR = right;
        const float mid = 0.5f * (dryL + dryR);
        const float drySide = 0.5f * (dryL - dryR);

        const float apL = processAllpassL (dryL);
        const float apR = processAllpassR (dryR);

        const float decorr = clampDecorrelate (decorrelateAmount);
        const float wetL = dryL + (apL - dryL) * decorr;
        const float wetR = dryR + (apR - dryR) * decorr;
        const float wetSide = 0.5f * (wetL - wetR);

        const float width = clampWidth (widthAmount);
        // Constant-power blend of dry vs decorrelated side, then scale by width.
        // width=0 → side 0 contribution from wet path and drySide*(1) … use:
        // side = drySide * cos(θ) + wetSide * sin(θ), θ = width * π/2
        // At width=0: side = drySide (passthrough). At width=1: side = wetSide.
        const float angle = width * 1.5707963267948966f;
        const float dryG = std::cos (angle);
        const float wetG = std::sin (angle);
        const float side = drySide * dryG + wetSide * wetG;

        left = mid + side;
        right = mid - side;
    }

private:
    double sampleRate_ = 44100.0;
    std::array<float, kDelayL> bufferL_ {};
    std::array<float, kDelayR> bufferR_ {};
    int writeL_ = 0;
    int writeR_ = 0;

    static float clamp01 (float v) noexcept
    {
        return std::max (0.0f, std::min (1.0f, v));
    }

    float processAllpassL (float input) noexcept
    {
        const float delayed = bufferL_[(std::size_t) writeL_];
        const float out = -kAllpassCoeff * input + delayed;
        bufferL_[(std::size_t) writeL_] = input + kAllpassCoeff * out;
        writeL_ = (writeL_ + 1) % kDelayL;
        return out;
    }

    float processAllpassR (float input) noexcept
    {
        const float delayed = bufferR_[(std::size_t) writeR_];
        const float out = -kAllpassCoeff * input + delayed;
        bufferR_[(std::size_t) writeR_] = input + kAllpassCoeff * out;
        writeR_ = (writeR_ + 1) % kDelayR;
        return out;
    }
};
