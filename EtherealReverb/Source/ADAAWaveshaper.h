#pragma once
#include <cmath>

/**
 * ADAAWaveshaper — first-order antiderivative antialiased tanh waveshaper.
 * N=1 uses ADAA (Parker et al. / ChowDSP); higher N falls back to tanh.
 * Audio-thread safe. No allocation after construction.
 * WoManus elite builtin — PearlLeash Plugins internal.
 */
template <int N = 1>
class ADAAWaveshaper
{
public:
    void prepare (double) noexcept { reset(); }

    void reset() noexcept
    {
        ad1Prev = 0.0f;
        xPrev = 0.0f;
    }

    inline float processSample (float x, float drive) noexcept
    {
        const float xDrive = x * drive;
        if constexpr (N == 1)
        {
            const float ad1 = antiDerivative1 (xDrive);
            const float denom = xDrive - xPrev + 1e-10f;
            const float out = (ad1 - ad1Prev) / denom;
            ad1Prev = ad1;
            xPrev = xDrive;
            return out;
        }
        return std::tanh (xDrive);
    }

    static float antiDerivative0 (float x) noexcept { return std::tanh (x); }
    static float antiDerivative1 (float x) noexcept
    {
        // log(cosh(x)) — clamp |x| to avoid overflow in cosh
        const float ax = std::abs (x);
        if (ax > 20.0f)
            return ax - std::log (2.0f);
        return std::log (std::cosh (x));
    }

private:
    float ad1Prev = 0.0f;
    float xPrev = 0.0f;
};
