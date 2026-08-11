#pragma once
#include <cmath>

/**
 * Canonical runtime one-pole smoother used by ClicklessBypass / EliteVST3Engine.
 * Class name is RuntimeParameterSmoother so it never collides with
 * WoManusSmoothedParameter (APVTS sm_* members in generated PluginProcessor).
 *
 * Compatibility: `using ParameterSmoother = RuntimeParameterSmoother` is the
 * single ParameterSmoother alias in the whole generated tree.
 */
class RuntimeParameterSmoother {
public:
    void init(float initialValue, float timeConstantMs, float sampleRate) {
        currentVal = initialValue;
        targetVal = initialValue;
        float timeConstantSec = timeConstantMs / 1000.0f;
        a = std::exp(-1.0f / (timeConstantSec * sampleRate));
    }
    void snapToValue(float value) { currentVal = value; targetVal = value; }
    inline void setTarget(float newTarget) { targetVal = newTarget; }
    inline float nextValue() {
        currentVal = targetVal + a * (currentVal - targetVal);
        return currentVal;
    }
    inline bool isSmoothing() const { return std::abs(targetVal - currentVal) > 1e-5f; }
private:
    float currentVal = 0.0f;
    float targetVal = 0.0f;
    float a = 0.0f;
};

/** Sole compatibility alias — do not re-alias in WoManusParameterSmoothing.h. */
using ParameterSmoother = RuntimeParameterSmoother;
