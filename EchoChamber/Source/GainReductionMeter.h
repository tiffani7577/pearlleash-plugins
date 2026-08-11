#pragma once
#include <cmath>

/**
 * GainReductionMeter — GR readout with 2 s peak hold and ~20 dB/s decay.
 * pushGainReduction() is audio-thread safe (plain floats, no locks).
 * UI reads getCurrentGR() / getPeakGR() (dB, typically ≤ 0).
 * WoManus elite UI builtin — PearlLeash Plugins internal.
 */
class GainReductionMeter
{
public:
    void prepare (double sampleRate)
    {
        const float sr = static_cast<float> (sampleRate > 0.0 ? sampleRate : 44100.0);
        // Decay toward 0 after hold — ~20 dB/s once released (coeff on linear-ish peak path).
        decayCoeff = std::exp (-1.0f / (0.05f * sr));
        holdSamples = static_cast<int> (2.0 * static_cast<double> (sr));
        currentGR = 0.0f;
        peakGR = 0.0f;
        holdCounter = 0;
    }

    void pushGainReduction (float grDb) noexcept
    {
        currentGR = grDb;
        if (grDb < peakGR)
        {
            peakGR = grDb;
            holdCounter = holdSamples;
        }

        if (holdCounter > 0)
            --holdCounter;
        else
            peakGR *= decayCoeff;
    }

    float getCurrentGR() const noexcept { return currentGR; }
    float getPeakGR() const noexcept { return peakGR; }

private:
    float currentGR = 0.0f;
    float peakGR = 0.0f;
    float decayCoeff = 0.0f;
    int holdSamples = 0;
    int holdCounter = 0;
};
