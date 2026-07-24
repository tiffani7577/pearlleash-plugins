#pragma once
#include <cmath>

class ParameterSmoother {
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
