#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>

/** Real-time FFT spectrum analyzer — pre/post EQ magnitudes for WebView EQ canvas. */
class WoManusAnalyzer
{
public:
    static constexpr int fftOrder = 11;
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int scopeSize = 512;

    WoManusAnalyzer() = default;

    void prepare (double sampleRate);
    void reset();

    /** Capture input buffer before DSP (call at start of processBlock). */
    void pushBuffer (const juce::AudioBuffer<float>& buffer);

    /** Capture output buffer after DSP (call at end of processBlock). */
    void pushPostBuffer (const juce::AudioBuffer<float>& buffer);

    /** dB magnitudes for WebView spectrum (512 bins). */
    std::vector<float> getPreEQMagnitudes() const;
    std::vector<float> getPostEQMagnitudes() const;

    /** JSON array literal e.g. [-48.2,-40.1,...] for evaluateJavascript. */
    juce::String getPreEQMagnitudesJsArray() const;
    juce::String getPostEQMagnitudesJsArray() const;

private:
    struct Channel
    {
        std::array<float, (size_t) fftSize> fifo {};
        std::array<float, (size_t) (2 * fftSize)> fftData {};
        int fifoIndex = 0;
        std::array<float, (size_t) scopeSize> magnitudes {};
    };

    void pushSamples (const juce::AudioBuffer<float>& buffer, Channel& channel);

    double sampleRate_ = 44100.0;
    juce::dsp::FFT forwardFFT_ { fftOrder };
    juce::dsp::WindowingFunction<float> window_ { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, true };

    Channel pre_;
    Channel post_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WoManusAnalyzer)
};
