#pragma once

#include <cmath>
#include <type_traits>

/** Exponential parameter smoother — 10 ms default, real-time safe (no heap). */
template <typename T>
class WoManusSmoothedParameter
{
public:
    WoManusSmoothedParameter() = default;

    void reset (T initialValue, float smoothTimeMs, double sampleRate)
    {
        current_ = initialValue;
        target_ = initialValue;
        smoothTimeMs_ = smoothTimeMs;
        update (sampleRate);
    }

    /** Legacy alias used by plugforge-runtime DSP helpers. */
    void init (T initialValue, float smoothTimeMs, float sampleRate)
    {
        reset (initialValue, smoothTimeMs, (double) sampleRate);
    }

    void setTarget (T newTarget) noexcept { target_ = newTarget; }

    /** Recompute smoothing coefficient when sample rate changes. */
    void update (double sampleRate) noexcept
    {
        const float sr = (float) (sampleRate > 0.0 ? sampleRate : 44100.0);
        const float timeSec = smoothTimeMs_ / 1000.0f;
        coeff_ = std::exp (-1.0f / (timeSec * sr));
    }

    T getNextValue() noexcept
    {
        current_ = target_ + coeff_ * (current_ - target_);
        return current_;
    }

    /** Legacy alias used by plugforge-runtime DSP helpers. */
    T nextValue() noexcept { return getNextValue(); }

    T getCurrentValue() const noexcept { return current_; }
    T getTargetValue() const noexcept { return target_; }

    bool isSmoothing() const noexcept
    {
        if constexpr (std::is_floating_point_v<T>)
            return std::abs (target_ - current_) > T (1e-5);
        return target_ != current_;
    }

    void snapToValue (T value) noexcept
    {
        current_ = value;
        target_ = value;
    }

private:
    T current_ {};
    T target_ {};
    float coeff_ { 0.0f };
    float smoothTimeMs_ { 10.0f };
};

using WoManusSmoothedFloat = WoManusSmoothedParameter<float>;

/** Back-compat alias for runtime headers (EliteVST3Engine, ClicklessBypass). */
using ParameterSmoother = WoManusSmoothedParameter<float>;
