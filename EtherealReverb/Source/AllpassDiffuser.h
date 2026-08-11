#pragma once

#include <array>
#include <cmath>

/**
 * Fixed eight-stage stereo allpass diffuser.
 *
 * Every stage and channel owns independent state. All storage is fixed-size,
 * prepare/reset perform no audio-thread work, and process() never allocates.
 */
class AllpassDiffuser
{
public:
    static constexpr int kStages = 8;
    static constexpr float kCoeff = 0.6f;

    void prepare (double) noexcept
    {
        reset();
    }

    void reset() noexcept
    {
        for (auto& buffer : buffersLeft) buffer.fill (0.0f);
        for (auto& buffer : buffersRight) buffer.fill (0.0f);
        positionsLeft.fill (0);
        positionsRight.fill (0);
    }

    void process (float& left, float& right) noexcept
    {
        for (int stage = 0; stage < kStages; ++stage)
        {
            left = allpass (
                left,
                buffersLeft[(std::size_t) stage],
                positionsLeft[(std::size_t) stage],
                delaySizes[(std::size_t) stage]);
            right = allpass (
                right,
                buffersRight[(std::size_t) stage],
                positionsRight[(std::size_t) stage],
                delaySizes[(std::size_t) stage]);
        }
    }

private:
    static constexpr int kMaximumDelay = 1200;
    inline static constexpr std::array<int, kStages> delaySizes {
        142, 107, 379, 277, 641, 503, 1153, 947
    };

    using DelayBuffer = std::array<float, kMaximumDelay>;
    std::array<DelayBuffer, kStages> buffersLeft {};
    std::array<DelayBuffer, kStages> buffersRight {};
    std::array<int, kStages> positionsLeft {};
    std::array<int, kStages> positionsRight {};

    static float allpass (
        float input,
        DelayBuffer& buffer,
        int& position,
        int delaySize) noexcept
    {
        const float delayed = buffer[(std::size_t) position];
        const float value = input - kCoeff * delayed;
        buffer[(std::size_t) position] = sanitize (value);
        position = (position + 1) % delaySize;
        return sanitize (delayed + kCoeff * value);
    }

    static float sanitize (float value) noexcept
    {
        if (! std::isfinite (value) || std::abs (value) < 1.0e-20f)
            return 0.0f;
        return value;
    }
};
