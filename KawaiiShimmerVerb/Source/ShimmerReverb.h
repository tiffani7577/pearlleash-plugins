#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

/**
 * Trusted stereo shimmer reverb.
 *
 * Design constraints:
 * - fixed base delay lengths: Bloom never moves a delay tap, so automation
 *   cannot create the large Doppler sweep produced by integer-delay resizing;
 * - independent state for every diffusion/FDN lane;
 * - octave-up feedback is additive, never a crossfade that removes the normal
 *   reverb excitation at shimmer=1;
 * - constant-power dry/wet law keeps the full-wet output audible;
 * - all storage is allocated in prepare(), never on the audio thread.
 */
class TrustedShimmerReverb
{
public:
    void prepare (double newSampleRate)
    {
        sampleRate = std::max (8000.0, newSampleRate);

        constexpr std::array<float, 8> fdnTimes {
            0.0297f, 0.0371f, 0.0411f, 0.0437f,
            0.0531f, 0.0617f, 0.0713f, 0.0899f
        };
        constexpr std::array<float, 4> diffuserTimes {
            0.0047f, 0.0063f, 0.0089f, 0.0127f
        };

        for (std::size_t lane = 0; lane < fdn.size(); ++lane)
        {
            fdn[lane].prepare ((int) std::ceil (sampleRate * 0.14) + 8);
            fdnDelaySamples[lane] = (float) sampleRate * fdnTimes[lane];
            dampingState[lane] = 0.0f;
            modulationPhase[lane] = (float) lane / (float) fdn.size();
        }

        for (std::size_t channel = 0; channel < diffusion.size(); ++channel)
        {
            for (std::size_t stage = 0; stage < diffusion[channel].size(); ++stage)
            {
                diffusion[channel][stage].prepare (
                    std::max (2, (int) std::round (sampleRate * diffuserTimes[stage])));
            }
        }

        for (auto& shifter : pitchShifter)
            shifter.prepare (sampleRate);

        reset();
        smoothingCoefficient = std::exp (-1.0f / (0.025f * (float) sampleRate));
    }

    void reset() noexcept
    {
        for (auto& delay : fdn) delay.reset();
        for (auto& channel : diffusion)
            for (auto& stage : channel)
                stage.reset();
        for (auto& shifter : pitchShifter) shifter.reset();
        dampingState.fill (0.0f);
        smoothedBloom = targetBloom;
        smoothedDecay = targetDecay;
        smoothedShimmer = targetShimmer;
        smoothedMix = targetMix;
    }

    void setParameters (float bloom, float decay, float shimmer, float mix) noexcept
    {
        targetBloom = clamp01 (bloom);
        targetDecay = clamp01 (decay);
        targetShimmer = clamp01 (shimmer);
        targetMix = clamp01 (mix);
    }

    void processSample (float& left, float& right) noexcept
    {
        advanceParameters();

        const float dryLeft = sanitize (left);
        const float dryRight = sanitize (right);
        const float diffusionCoefficient = 0.52f + 0.20f * smoothedBloom;

        float diffuseLeft = dryLeft;
        float diffuseRight = dryRight;
        for (auto& stage : diffusion[0])
            diffuseLeft = stage.process (diffuseLeft, diffusionCoefficient);
        for (auto& stage : diffusion[1])
            diffuseRight = stage.process (diffuseRight, diffusionCoefficient);

        const float modulationDepth = 0.65f + 0.55f * smoothedBloom;
        for (std::size_t lane = 0; lane < fdn.size(); ++lane)
        {
            const float rateHz = 0.071f + 0.013f * (float) lane;
            modulationPhase[lane] += rateHz / (float) sampleRate;
            if (modulationPhase[lane] >= 1.0f) modulationPhase[lane] -= 1.0f;
            const float modulation =
                std::sin (twoPi * modulationPhase[lane]) * modulationDepth;
            fdnReadScratch[lane] =
                fdn[lane].read (fdnDelaySamples[lane] + modulation);
        }

        // Decorrelated stereo output from all eight FDN lanes.
        float wetLeft =
            (fdnReadScratch[0] + fdnReadScratch[2]
             + fdnReadScratch[4] + fdnReadScratch[6]
             - 0.35f * (fdnReadScratch[1] + fdnReadScratch[5])) * 0.34f;
        float wetRight =
            (fdnReadScratch[1] + fdnReadScratch[3]
             + fdnReadScratch[5] + fdnReadScratch[7]
             - 0.35f * (fdnReadScratch[2] + fdnReadScratch[6])) * 0.34f;

        const float octaveLeft = pitchShifter[0].process (wetLeft);
        const float octaveRight = pitchShifter[1].process (wetRight);

        // Orthonormal 8x8 Hadamard feedback matrix. The 1/sqrt(8)
        // normalization preserves energy before decay/damping are applied.
        fdnMixScratch.fill (0.0f);
        for (std::size_t row = 0; row < fdn.size(); ++row)
        {
            for (std::size_t column = 0; column < fdn.size(); ++column)
                fdnMixScratch[row] +=
                    hadamard[row][column] * fdnReadScratch[column];
        }

        const float feedback = 0.58f + 0.385f * smoothedDecay;
        // advanceParameters() ran at the start of this sample, so this
        // coefficient follows the smoother per sample rather than raw block data.
        const float dampCoeff = 0.18f + 0.46f * (1.0f - smoothedBloom);
        for (std::size_t lane = 0; lane < fdn.size(); ++lane)
        {
            dampingState[lane] +=
                dampCoeff * (fdnMixScratch[lane] - dampingState[lane]);

            const bool leftLane = (lane & 1u) == 0u;
            const float direct = leftLane ? diffuseLeft : diffuseRight;
            const float cross = leftLane ? diffuseRight : diffuseLeft;
            const float octave = leftLane ? octaveLeft : octaveRight;
            const float sign = (lane & 2u) == 0u ? 1.0f : -1.0f;

            // Shimmer is additive. The direct excitation remains present at 1.0.
            const float excitation =
                direct * 0.42f + cross * 0.08f
                + octave * smoothedShimmer * 0.34f * sign;
            fdn[lane].write (sanitize (excitation + dampingState[lane] * feedback));
        }

        // Constant-power blend; wet compensation preserves level at mix=1.
        const float dryGain = std::cos (smoothedMix * halfPi);
        const float wetGain = std::sin (smoothedMix * halfPi) * 1.12f;
        left = sanitize (dryLeft * dryGain + wetLeft * wetGain);
        right = sanitize (dryRight * dryGain + wetRight * wetGain);
    }

private:
    static constexpr float pi = 3.14159265358979323846f;
    static constexpr float twoPi = 2.0f * pi;
    static constexpr float halfPi = 0.5f * pi;
    inline static constexpr float hadamard[8][8] {
        {  0.35355339f,  0.35355339f,  0.35355339f,  0.35355339f,
           0.35355339f,  0.35355339f,  0.35355339f,  0.35355339f },
        {  0.35355339f, -0.35355339f,  0.35355339f, -0.35355339f,
           0.35355339f, -0.35355339f,  0.35355339f, -0.35355339f },
        {  0.35355339f,  0.35355339f, -0.35355339f, -0.35355339f,
           0.35355339f,  0.35355339f, -0.35355339f, -0.35355339f },
        {  0.35355339f, -0.35355339f, -0.35355339f,  0.35355339f,
           0.35355339f, -0.35355339f, -0.35355339f,  0.35355339f },
        {  0.35355339f,  0.35355339f,  0.35355339f,  0.35355339f,
          -0.35355339f, -0.35355339f, -0.35355339f, -0.35355339f },
        {  0.35355339f, -0.35355339f,  0.35355339f, -0.35355339f,
          -0.35355339f,  0.35355339f, -0.35355339f,  0.35355339f },
        {  0.35355339f,  0.35355339f, -0.35355339f, -0.35355339f,
          -0.35355339f, -0.35355339f,  0.35355339f,  0.35355339f },
        {  0.35355339f, -0.35355339f, -0.35355339f,  0.35355339f,
          -0.35355339f,  0.35355339f,  0.35355339f, -0.35355339f }
    };

    static float clamp01 (float value) noexcept
    {
        return std::max (0.0f, std::min (1.0f, value));
    }

    static float sanitize (float value) noexcept
    {
        if (! std::isfinite (value) || std::abs (value) < 1.0e-20f)
            return 0.0f;
        return std::max (-8.0f, std::min (8.0f, value));
    }

    class FractionalDelay
    {
    public:
        void prepare (int maximumDelaySamples)
        {
            buffer.assign ((std::size_t) std::max (8, maximumDelaySamples + 4), 0.0f);
            writeIndex = 0;
        }

        void reset() noexcept
        {
            std::fill (buffer.begin(), buffer.end(), 0.0f);
            writeIndex = 0;
        }

        void write (float value) noexcept
        {
            if (buffer.empty()) return;
            buffer[writeIndex] = value;
            writeIndex = (writeIndex + 1u) % buffer.size();
        }

        float read (float delaySamples) const noexcept
        {
            if (buffer.empty()) return 0.0f;
            const float maximum = (float) buffer.size() - 3.0f;
            const float delay = std::max (1.0f, std::min (maximum, delaySamples));
            float position = (float) writeIndex - delay;
            while (position < 0.0f) position += (float) buffer.size();
            const std::size_t index0 = (std::size_t) position % buffer.size();
            const std::size_t index1 = (index0 + 1u) % buffer.size();
            const float fraction = position - std::floor (position);
            return buffer[index0] + (buffer[index1] - buffer[index0]) * fraction;
        }

    private:
        std::vector<float> buffer;
        std::size_t writeIndex = 0;
    };

    class AllpassDiffuser
    {
    public:
        void prepare (int delaySamples)
        {
            buffer.assign ((std::size_t) std::max (2, delaySamples + 1), 0.0f);
            index = 0;
        }

        void reset() noexcept
        {
            std::fill (buffer.begin(), buffer.end(), 0.0f);
            index = 0;
        }

        float process (float input, float coefficient) noexcept
        {
            if (buffer.empty()) return input;
            const float delayed = buffer[index];
            const float output = delayed - coefficient * input;
            buffer[index] = sanitize (input + coefficient * output);
            index = (index + 1u) % buffer.size();
            return sanitize (output);
        }

    private:
        std::vector<float> buffer;
        std::size_t index = 0;
    };

    class OctavePitchShifter
    {
    public:
        void prepare (double newSampleRate)
        {
            const int requestedWindow = (int) std::round (newSampleRate * 0.046f);
            windowSamples = std::max (512, std::min (4096, requestedWindow));
            delay.prepare (windowSamples + 16);
            phase = 0.0f;
        }

        void reset() noexcept
        {
            delay.reset();
            phase = 0.0f;
        }

        float process (float input) noexcept
        {
            delay.write (sanitize (input));
            const float secondPhase = phase < 0.5f ? phase + 0.5f : phase - 0.5f;
            const float firstWeight = 0.5f - 0.5f * std::cos (twoPi * phase);
            const float secondWeight = 1.0f - firstWeight;
            const float firstDelay = 2.0f + (1.0f - phase) * (float) windowSamples;
            const float secondDelay =
                2.0f + (1.0f - secondPhase) * (float) windowSamples;
            const float output =
                delay.read (firstDelay) * firstWeight
                + delay.read (secondDelay) * secondWeight;
            phase += 1.0f / (float) windowSamples; // 2× read speed => +12 semitones.
            if (phase >= 1.0f) phase -= 1.0f;
            return sanitize (output);
        }

    private:
        FractionalDelay delay;
        int windowSamples = 2048;
        float phase = 0.0f;
    };

    void advanceParameters() noexcept
    {
        const float oneMinus = 1.0f - smoothingCoefficient;
        smoothedBloom = targetBloom + smoothingCoefficient * (smoothedBloom - targetBloom);
        smoothedDecay = targetDecay + smoothingCoefficient * (smoothedDecay - targetDecay);
        smoothedShimmer =
            targetShimmer + smoothingCoefficient * (smoothedShimmer - targetShimmer);
        smoothedMix = targetMix + smoothingCoefficient * (smoothedMix - targetMix);
        (void) oneMinus;
    }

    double sampleRate = 44100.0;
    float smoothingCoefficient = 0.999f;
    float targetBloom = 0.55f;
    float targetDecay = 0.72f;
    float targetShimmer = 0.45f;
    float targetMix = 0.5f;
    float smoothedBloom = targetBloom;
    float smoothedDecay = targetDecay;
    float smoothedShimmer = targetShimmer;
    float smoothedMix = targetMix;

    std::array<FractionalDelay, 8> fdn;
    std::array<float, 8> fdnDelaySamples {};
    std::array<float, 8> dampingState {};
    std::array<float, 8> modulationPhase {};
    std::array<float, 8> fdnReadScratch {};
    std::array<float, 8> fdnMixScratch {};
    std::array<std::array<AllpassDiffuser, 4>, 2> diffusion;
    std::array<OctavePitchShifter, 2> pitchShifter;
};
