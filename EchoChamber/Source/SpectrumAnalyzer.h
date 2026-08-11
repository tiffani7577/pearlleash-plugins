#pragma once
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <memory>

/**
 * SpectrumAnalyzer — real-time FFT magnitude analyzer (Hann, 2048).
 * Audio thread: pushSample() (lock-free FIFO + atomic ready flag).
 * UI thread (~60 Hz): processIfReady() then getBinDb().
 * No heap after prepare(). WoManus elite UI builtin — PearlLeash Plugins internal.
 */
class SpectrumAnalyzer
{
public:
    static constexpr int fftSize = 2048;
    static constexpr int fftOrder = 11; // 2^11 = 2048

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        forwardFFT = std::make_unique<juce::dsp::FFT> (fftOrder);
        window = std::make_unique<juce::dsp::WindowingFunction<float>> (
            static_cast<size_t> (fftSize),
            juce::dsp::WindowingFunction<float>::hann);
        fifo.fill (0.0f);
        fftData.fill (0.0f);
        smoothedData.fill (0.0f);
        fifoIndex = 0;
        nextFFTBlockReady.store (false, std::memory_order_release);
    }

    /** Call from processBlock — audio-thread safe (no locks, no alloc). */
    void pushSample (float sample) noexcept
    {
        if (fifoIndex == fftSize)
        {
            if (! nextFFTBlockReady.load (std::memory_order_acquire))
            {
                juce::FloatVectorOperations::copy (fftData.data(), fifo.data(), fftSize);
                nextFFTBlockReady.store (true, std::memory_order_release);
            }
            fifoIndex = 0;
        }

        fifo[static_cast<size_t> (fifoIndex++)] = sample;
    }

    /** Call from UI thread at ~60 Hz. Returns true when a new FFT was computed. */
    bool processIfReady()
    {
        if (! nextFFTBlockReady.load (std::memory_order_acquire))
            return false;

        if (window != nullptr)
            window->multiplyWithWindowingTable (fftData.data(), static_cast<size_t> (fftSize));

        if (forwardFFT != nullptr)
            forwardFFT->performFrequencyOnlyForwardTransform (fftData.data());

        constexpr float smooth = 0.85f;
        constexpr float attack = 0.15f;
        for (size_t i = 0; i < smoothedData.size(); ++i)
            smoothedData[i] = smoothedData[i] * smooth + fftData[i] * attack;

        nextFFTBlockReady.store (false, std::memory_order_release);
        return true;
    }

    float getBinDb (int bin) const
    {
        const int n = getNumBins();
        if (bin < 0 || bin >= n)
            return -80.0f;
        return juce::Decibels::gainToDecibels (smoothedData[static_cast<size_t> (bin)], -80.0f);
    }

    int getNumBins() const noexcept { return fftSize / 2; }
    double getSampleRate() const noexcept { return sampleRate; }

private:
    std::unique_ptr<juce::dsp::FFT> forwardFFT;
    std::unique_ptr<juce::dsp::WindowingFunction<float>> window;
    std::array<float, fftSize> fifo {};
    std::array<float, fftSize * 2> fftData {};
    std::array<float, fftSize> smoothedData {};
    int fifoIndex = 0;
    std::atomic<bool> nextFFTBlockReady { false };
    double sampleRate = 44100.0;
};
