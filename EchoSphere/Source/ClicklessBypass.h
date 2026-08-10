#pragma once
// PlugForge Runtime — latency-aligned dry/wet bypass crossfade.

#include "RuntimeParameterSmoother.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>

class ClicklessBypass
{
public:
    void prepare (double sampleRate, int maximumBlockSize, int latencySamplesToMatch)
    {
        latencySamples = juce::jmax (0, latencySamplesToMatch);
        dryDelay.setMaximumDelayInSamples (juce::jmax (1, latencySamples + 1));
        // JUCE DelayLine only allocates channel/writePos buffers in prepare() —
        // setMaximumDelayInSamples alone leaves channel count at 0 → SIGSEGV on
        // the first popSample/pushSample (RenderTest / host processBlock).
        juce::dsp::ProcessSpec spec {
            sampleRate > 0.0 ? sampleRate : 44100.0,
            (juce::uint32) juce::jmax (1, maximumBlockSize),
            2u
        };
        dryDelay.prepare (spec);
        dryDelay.setDelay ((float) latencySamples);
        fade.init (bypassed ? 0.0f : 1.0f, 5.0f, (float) sampleRate);
        delayPrepared = true;
    }

    void setBypassed (bool shouldBypass) noexcept
    {
        bypassed = shouldBypass;
        fade.setTarget (shouldBypass ? 0.0f : 1.0f);
    }

    bool isBypassed() const noexcept { return bypassed; }

    /** Mix delayed dry with processed wet. wetGain from fade (1 = fully wet). */
    void process (juce::AudioBuffer<float>& dryInOut,
                  const juce::AudioBuffer<float>& wet) noexcept
    {
        juce::ScopedNoDenormals noDenormals;
        // Never walk past the shorter buffer (dry staging is often oversized).
        const int numSamples = juce::jmin (dryInOut.getNumSamples(), wet.getNumSamples());
        const int numChannels = juce::jmin (dryInOut.getNumChannels(), wet.getNumChannels());
        // Stale / unprepared DelayLine must never call popSample (SIGSEGV).
        const bool useDelay = latencySamples > 0 && delayPrepared;

        for (int i = 0; i < numSamples; ++i)
        {
            const float wetGain = fade.nextValue();
            const float dryGain = 1.0f - wetGain;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                // Host / fixture may inject NaN (nan_recovery). Never feed DelayLine or mix.
                float drySample = dryInOut.getSample (ch, i);
                if (! std::isfinite (drySample))
                    drySample = 0.0f;

                float delayedDry = drySample;
                if (useDelay)
                {
                    delayedDry = dryDelay.popSample (ch);
                    if (! std::isfinite (delayedDry))
                        delayedDry = 0.0f;
                    dryDelay.pushSample (ch, drySample);
                }

                float wetSample = wet.getSample (ch, i);
                if (! std::isfinite (wetSample))
                    wetSample = 0.0f;

                const float mixed = delayedDry * dryGain + wetSample * wetGain;
                dryInOut.setSample (ch, i, std::isfinite (mixed) ? mixed : 0.0f);
            }
        }
    }

private:
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dryDelay { 8192 };
    RuntimeParameterSmoother fade;
    int latencySamples = 0;
    bool bypassed = false;
    bool delayPrepared = false;
};
