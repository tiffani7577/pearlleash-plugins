#pragma once
#include <algorithm>
#include <cmath>

struct SignalSanityGuard {
    void prepare(double) noexcept {}

    static inline float sanitize(float sample) {
        if (std::isnan(sample) || std::isinf(sample)) return 0.0f;
        if (std::abs(sample) < 1e-30f) return 0.0f;
        return std::clamp(sample, -1.5f, 1.5f);
    }
};
