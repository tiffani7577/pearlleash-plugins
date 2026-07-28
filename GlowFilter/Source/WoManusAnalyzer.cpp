#include "WoManusAnalyzer.h"

void WoManusAnalyzer::prepare (double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    reset();
}

void WoManusAnalyzer::reset()
{
    pre_ = {};
    post_ = {};
    pre_.magnitudes.fill (-100.0f);
    post_.magnitudes.fill (-100.0f);
}

void WoManusAnalyzer::pushBuffer (const juce::AudioBuffer<float>& buffer)
{
    pushSamples (buffer, pre_);
}

void WoManusAnalyzer::pushPostBuffer (const juce::AudioBuffer<float>& buffer)
{
    pushSamples (buffer, post_);
}

void WoManusAnalyzer::pushSamples (const juce::AudioBuffer<float>& buffer, Channel& channel)
{
    if (buffer.getNumSamples() <= 0)
        return;

    const int numCh = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    for (int i = 0; i < numSamples; ++i)
    {
        float mono = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            mono += buffer.getSample (ch, i);
        mono /= (float) juce::jmax (1, numCh);

        channel.fifo[(size_t) channel.fifoIndex] = mono;
        channel.fifoIndex = (channel.fifoIndex + 1) % fftSize;

        if (channel.fifoIndex != 0)
            continue;

        std::copy (channel.fifo.begin(), channel.fifo.end(), channel.fftData.begin());
        window_.multiplyWithWindowingTable (channel.fftData.data(), (size_t) fftSize);
        forwardFFT_.performFrequencyOnlyForwardTransform (channel.fftData.data());

        const int usableBins = fftSize / 2;
        const float binToScope = (float) usableBins / (float) scopeSize;

        for (int b = 0; b < scopeSize; ++b)
        {
            const int bin = juce::jlimit (0, usableBins - 1, (int) std::floor ((float) b * binToScope));
            const float mag = channel.fftData[(size_t) bin];
            channel.magnitudes[(size_t) b] =
                juce::Decibels::gainToDecibels (juce::jmax (1.0e-9f, mag), -100.0f);
        }
    }

    juce::ignoreUnused (sampleRate_);
}

std::vector<float> WoManusAnalyzer::getPreEQMagnitudes() const
{
    return { pre_.magnitudes.begin(), pre_.magnitudes.end() };
}

std::vector<float> WoManusAnalyzer::getPostEQMagnitudes() const
{
    return { post_.magnitudes.begin(), post_.magnitudes.end() };
}

juce::String WoManusAnalyzer::getPreEQMagnitudesJsArray() const
{
    juce::String s = "[";
    for (int i = 0; i < scopeSize; ++i)
    {
        if (i > 0) s << ",";
        s << juce::String (pre_.magnitudes[(size_t) i], 2);
    }
    s << "]";
    return s;
}

juce::String WoManusAnalyzer::getPostEQMagnitudesJsArray() const
{
    juce::String s = "[";
    for (int i = 0; i < scopeSize; ++i)
    {
        if (i > 0) s << ",";
        s << juce::String (post_.magnitudes[(size_t) i], 2);
    }
    s << "]";
    return s;
}
