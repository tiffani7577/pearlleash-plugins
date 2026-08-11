#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

/**
 * AirShelf — high-shelf “air” band (~12 kHz, +2 dB, Q≈0.8).
 * Final-stage polish for saturation / tape / tube chains.
 * Audio-thread safe. No allocation after prepare().
 * WoManus elite builtin — PearlLeash Plugins internal.
 */
class AirShelf
{
public:
    void prepare (float sampleRate) noexcept
    {
        const float sr = juce::jmax (1.0f, sampleRate);
        const float A = std::pow (10.0f, 2.0f / 40.0f);
        const float w0 = 2.0f * juce::MathConstants<float>::pi * 12000.0f / sr;
        const float cosw = std::cos (w0);
        const float sinw = std::sin (w0);
        const float alpha = sinw / (2.0f * 0.8f);
        const float sqrtA = std::sqrt (A);
        const float a0c = (A + 1.0f) + (A - 1.0f) * cosw + 2.0f * sqrtA * alpha;
        b0 = (A * ((A + 1.0f) - (A - 1.0f) * cosw + 2.0f * sqrtA * alpha)) / a0c;
        b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw)) / a0c;
        b2 = (A * ((A + 1.0f) - (A - 1.0f) * cosw - 2.0f * sqrtA * alpha)) / a0c;
        a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cosw)) / a0c;
        a2 = ((A + 1.0f) + (A - 1.0f) * cosw - 2.0f * sqrtA * alpha) / a0c;
        reset();
    }

    void prepare (double sampleRate) noexcept
    {
        prepare (static_cast<float> (sampleRate));
    }

    void reset() noexcept
    {
        x1 = x2 = y1 = y2 = 0.0f;
    }

    inline float processSample (float x) noexcept
    {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }

private:
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
};
