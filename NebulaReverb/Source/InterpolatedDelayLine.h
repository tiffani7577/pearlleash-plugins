#pragma once
// WoManus Platform v5 | PearlLeash | Copyright 2026 Tiffani LeBlanc
//
// Shared time-domain delay primitive for delay / reverb predelay / chorus.
// Phase A1: selectable interpolation, independent stereo lines, bounded
// stereo offset, optional dual-tap. Zero heap allocation after prepare().
// Feedback saturation / ADAA / diffusion are intentionally out of scope.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

/** Fractional-delay read quality. Cubic (Lagrange) remains the default. */
enum class DelayInterpolationQuality : int
{
    Hold = 0,
    Linear = 1,
    Cubic = 2,
    Hermite = 3
};

/**
 * Single-channel fractional delay line.
 * prepare() may allocate; writeSample / readSample / reset are audio-thread safe
 * and never allocate.
 */
class InterpolatedDelayLine
{
public:
    void prepare (int maxDelaySamples)
    {
        const int size = std::max (8, maxDelaySamples + 5);
        buffer.assign ((std::size_t) size, 0.0f);
        writeIndex = 0;
        maxDelay = size - 5;
    }

    void setInterpolationQuality (DelayInterpolationQuality quality) noexcept
    {
        interpQuality = quality;
    }

    DelayInterpolationQuality getInterpolationQuality() const noexcept
    {
        return interpQuality;
    }

    int getMaxDelaySamples() const noexcept
    {
        return maxDelay;
    }

    int getBufferSize() const noexcept
    {
        return (int) buffer.size();
    }

    void writeSample (float input) noexcept
    {
        if (buffer.empty())
            return;
        buffer[(std::size_t) writeIndex] = input;
        writeIndex = (writeIndex + 1) % (int) buffer.size();
    }

    float readSample (float delaySamples) const noexcept
    {
        if (buffer.empty())
            return 0.0f;
        const float clamped = clampDelay (delaySamples);
        float readPos = (float) writeIndex - clamped;
        const float size = (float) buffer.size();
        while (readPos < 0.0f)
            readPos += size;
        while (readPos >= size)
            readPos -= size;
        return interpolateAt (readPos);
    }

    /**
     * Dual-tap read: primary delay plus a secondary tap offset by tap2OffsetSamples.
     * tap2Mix in [0,1] blends primary↔secondary (0 = primary only).
     */
    float readDualTap (float delaySamples, float tap2OffsetSamples, float tap2Mix) const noexcept
    {
        const float a = readSample (delaySamples);
        const float mix = clamp01 (tap2Mix);
        if (mix <= 0.0f)
            return a;
        const float b = readSample (delaySamples + tap2OffsetSamples);
        return a * (1.0f - mix) + b * mix;
    }

    void reset() noexcept
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
    }

private:
    std::vector<float> buffer;
    int writeIndex = 0;
    int maxDelay = 0;
    DelayInterpolationQuality interpQuality = DelayInterpolationQuality::Cubic;

    static float clamp01 (float v) noexcept
    {
        return std::max (0.0f, std::min (1.0f, v));
    }

    float clampDelay (float delaySamples) const noexcept
    {
        const float hi = (float) std::max (1, maxDelay);
        return std::max (1.0f, std::min (hi, delaySamples));
    }

    float sampleAt (int index) const noexcept
    {
        const int n = (int) buffer.size();
        int i = index % n;
        if (i < 0)
            i += n;
        return buffer[(std::size_t) i];
    }

    float interpolateAt (float readPos) const noexcept
    {
        const int i0 = (int) readPos;
        const float frac = readPos - (float) i0;

        switch (interpQuality)
        {
            case DelayInterpolationQuality::Hold:
                return sampleAt (i0);

            case DelayInterpolationQuality::Linear:
            {
                const float y0 = sampleAt (i0);
                const float y1 = sampleAt (i0 + 1);
                return y0 + frac * (y1 - y0);
            }

            case DelayInterpolationQuality::Hermite:
            {
                // Catmull-Rom / 4-point Hermite.
                const float yM1 = sampleAt (i0 - 1);
                const float y0 = sampleAt (i0);
                const float y1 = sampleAt (i0 + 1);
                const float y2 = sampleAt (i0 + 2);
                const float c0 = y0;
                const float c1 = 0.5f * (y1 - yM1);
                const float c2 = yM1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
                const float c3 = 0.5f * (y2 - yM1) + 1.5f * (y0 - y1);
                return ((c3 * frac + c2) * frac + c1) * frac + c0;
            }

            case DelayInterpolationQuality::Cubic:
            default:
            {
                // Third-order Lagrange (legacy default).
                const float yM1 = sampleAt (i0 - 1);
                const float y0 = sampleAt (i0);
                const float y1 = sampleAt (i0 + 1);
                const float y2 = sampleAt (i0 + 2);
                const float cM1 = -frac * (frac - 1.0f) * (frac - 2.0f) / 6.0f;
                const float c0 = (frac + 1.0f) * (frac - 1.0f) * (frac - 2.0f) * 0.5f;
                const float c1 = -(frac + 1.0f) * frac * (frac - 2.0f) * 0.5f;
                const float c2 = (frac + 1.0f) * frac * (frac - 1.0f) / 6.0f;
                return (yM1 * cM1) + (y0 * c0) + (y1 * c1) + (y2 * c2);
            }
        }
    }
};

/**
 * Independent L/R delay structures with bounded stereo offset and optional dual-tap.
 * Mono in → identical L/R out when stereoOffsetMs == 0 and dualTap off (mono compatible).
 */
class StereoInterpolatedDelayPair
{
public:
    static constexpr float kMaxStereoOffsetMs = 25.0f;
    static constexpr float kDefaultMaxDelaySeconds = 1.25f;

    void prepare (double sampleRate, float maxDelaySeconds = kDefaultMaxDelaySeconds)
    {
        sampleRate_ = std::max (8000.0, sampleRate);
        const float seconds = std::max (0.05f, maxDelaySeconds);
        maxDelaySamples_ = std::max (8, (int) std::ceil (sampleRate_ * (double) seconds) + 8);
        left_.prepare (maxDelaySamples_);
        right_.prepare (maxDelaySamples_);
        left_.setInterpolationQuality (interpQuality_);
        right_.setInterpolationQuality (interpQuality_);
    }

    void setInterpolationQuality (DelayInterpolationQuality quality) noexcept
    {
        interpQuality_ = quality;
        left_.setInterpolationQuality (quality);
        right_.setInterpolationQuality (quality);
    }

    DelayInterpolationQuality getInterpolationQuality() const noexcept
    {
        return interpQuality_;
    }

    void reset() noexcept
    {
        left_.reset();
        right_.reset();
    }

    InterpolatedDelayLine& leftLine() noexcept { return left_; }
    InterpolatedDelayLine& rightLine() noexcept { return right_; }
    const InterpolatedDelayLine& leftLine() const noexcept { return left_; }
    const InterpolatedDelayLine& rightLine() const noexcept { return right_; }

    /** Bound stereo offset to ±kMaxStereoOffsetMs. */
    static float clampStereoOffsetMs (float offsetMs) noexcept
    {
        return std::max (-kMaxStereoOffsetMs, std::min (kMaxStereoOffsetMs, offsetMs));
    }

    /**
     * Resolve per-channel delay times (ms).
     * L = center, R = center + offset (offset may be negative).
     * Both clamped to [minMs, maxMs].
     */
    static void resolveChannelDelayMs (
        float centerMs,
        float stereoOffsetMs,
        float minMs,
        float maxMs,
        float& outLeftMs,
        float& outRightMs) noexcept
    {
        const float offset = clampStereoOffsetMs (stereoOffsetMs);
        outLeftMs = std::max (minMs, std::min (maxMs, centerMs));
        outRightMs = std::max (minMs, std::min (maxMs, centerMs + offset));
    }

    /**
     * Process one stereo frame through independent delay lines.
     * Does not allocate. feedback/mix are plain floats (host smoothing is external).
     */
    void processSample (
        float& left,
        float& right,
        float delayMs,
        float stereoOffsetMs,
        float feedback,
        float mix,
        bool dualTap = false,
        float tap2OffsetMs = 12.0f,
        float tap2Mix = 0.35f) noexcept
    {
        const float fb = std::max (0.0f, std::min (0.95f, feedback));
        const float wetMix = std::max (0.0f, std::min (1.0f, mix));
        float leftMs = delayMs;
        float rightMs = delayMs;
        resolveChannelDelayMs (delayMs, stereoOffsetMs, 1.0f, maxDelayMs(), leftMs, rightMs);

        const float leftDelaySamples = msToSamples (leftMs);
        const float rightDelaySamples = msToSamples (rightMs);
        const float tap2Samples = msToSamples (std::max (0.0f, tap2OffsetMs));
        const float t2mix = dualTap ? std::max (0.0f, std::min (1.0f, tap2Mix)) : 0.0f;

        const float dryL = left;
        const float dryR = right;

        const float wetL = dualTap
            ? left_.readDualTap (leftDelaySamples, tap2Samples, t2mix)
            : left_.readSample (leftDelaySamples);
        const float wetR = dualTap
            ? right_.readDualTap (rightDelaySamples, tap2Samples, t2mix)
            : right_.readSample (rightDelaySamples);

        left_.writeSample (dryL + wetL * fb);
        right_.writeSample (dryR + wetR * fb);

        left = dryL * (1.0f - wetMix) + wetL * wetMix;
        right = dryR * (1.0f - wetMix) + wetR * wetMix;
    }

    /**
     * Same as processSample, but routes feedback through a StereoFeedbackNonlinearIsland
     * (Phase A3). feedbackAmount still applied inside the island.
     */
    template <typename StereoFbIsland>
    void processSampleWithFeedbackIsland (
        float& left,
        float& right,
        float delayMs,
        float stereoOffsetMs,
        float feedback,
        float mix,
        StereoFbIsland& fbIsland,
        float fbDrive,
        float fbDamp,
        bool fbSatEnable,
        int fbOsMode,
        bool dualTap = false,
        float tap2OffsetMs = 12.0f,
        float tap2Mix = 0.35f) noexcept
    {
        const float wetMix = std::max (0.0f, std::min (1.0f, mix));
        float leftMs = delayMs;
        float rightMs = delayMs;
        resolveChannelDelayMs (delayMs, stereoOffsetMs, 1.0f, maxDelayMs(), leftMs, rightMs);

        const float leftDelaySamples = msToSamples (leftMs);
        const float rightDelaySamples = msToSamples (rightMs);
        const float tap2Samples = msToSamples (std::max (0.0f, tap2OffsetMs));
        const float t2mix = dualTap ? std::max (0.0f, std::min (1.0f, tap2Mix)) : 0.0f;

        const float dryL = left;
        const float dryR = right;

        const float wetL = dualTap
            ? left_.readDualTap (leftDelaySamples, tap2Samples, t2mix)
            : left_.readSample (leftDelaySamples);
        const float wetR = dualTap
            ? right_.readDualTap (rightDelaySamples, tap2Samples, t2mix)
            : right_.readSample (rightDelaySamples);

        const float fbL = fbIsland.processLeft (
            wetL, feedback, fbDrive, fbDamp, fbSatEnable, fbOsMode);
        const float fbR = fbIsland.processRight (
            wetR, feedback, fbDrive, fbDamp, fbSatEnable, fbOsMode);

        left_.writeSample (dryL + fbL);
        right_.writeSample (dryR + fbR);

        left = dryL * (1.0f - wetMix) + wetL * wetMix;
        right = dryR * (1.0f - wetMix) + wetR * wetMix;
    }

    float maxDelayMs() const noexcept
    {
        if (sampleRate_ <= 0.0)
            return 1.0f;
        return (float) ((double) std::max (1, maxDelaySamples_) * 1000.0 / sampleRate_);
    }

    double getSampleRate() const noexcept { return sampleRate_; }

private:
    InterpolatedDelayLine left_;
    InterpolatedDelayLine right_;
    double sampleRate_ = 44100.0;
    int maxDelaySamples_ = 0;
    DelayInterpolationQuality interpQuality_ = DelayInterpolationQuality::Cubic;

    float msToSamples (float ms) const noexcept
    {
        return (float) (std::max (0.0f, ms) * 0.001 * sampleRate_);
    }
};
