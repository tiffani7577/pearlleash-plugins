#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include "PlugForgeDenormals.h"
#include "ClicklessBypass.h"
#include "WoManusParameterSmoothing.h"
#include "ParameterVersioning.h"




#include "ShimmerReverb.h"

#include "AllpassDiffuser.h"

#include "PhaseDecorrelator.h"
#include "SignalSanityGuard.h"

#include "AirShelf.h"
#include "SpectrumAnalyzer.h"





#include "WoManusAnalyzer.h"
#include "CloudSyncClient.h"
#include "License.h"



class TruePeakLimiter {
public:
    void prepare(float sampleRate) {
        threshold = 0.9440f; // -0.3 dBFS
        release = std::exp(-1.0f / (sampleRate * 0.1f));
        gain = 1.0f;
    }

    inline float processSample(float x) {
        float absX = std::abs(x);
        if (absX * gain > threshold)
            gain = threshold / absX;
        else
            gain += (1.0f - gain) * (1.0f - release);
        gain = juce::jlimit(0.0f, 1.0f, gain);
        return x * gain;
    }

    void reset() { gain = 1.0f; }

private:
    float threshold = 0.9440f;
    float release = 0.999f;
    float gain = 1.0f;
};


class EchoChamberProcessor : public juce::AudioProcessor, public juce::AudioProcessorValueTreeState::Listener
{
public:
    EchoChamberProcessor()
        : juce::AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
    {
        // Portable FTZ (x86 MXCSR / arm64 FPCR). processBlock also uses juce::ScopedNoDenormals.
        pfEnableFlushToZero();

        licensed = EchoChamberLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("EchoChamber: unlicensed copy — enter your serial to activate.");
        // Living plugin cloud sync — prepare only. Do NOT startSync() in the processor ctor:
        // auval/Logic construct many AU instances during validation; a background thread +
        // stopThread(2000) in every dtor makes the host report the AU as unstable.
        cloudSync.prepare ("com.pearlleash.echochamber",
                           juce::String(),
                           "1.0.0");
        versionManager.registerParameter ("mix", 0.35f, 1);
        versionManager.registerParameter ("predelay", 20.0f, 1);
        versionManager.registerParameter ("gain", 0.0f, 1);
        versionManager.registerParameter ("bypass", 0.0f, 1);
        versionManager.registerParameter ("width", 0.35f, 1);
        versionManager.registerParameter ("decorrelate", 0.7f, 1);


    }

    ~EchoChamberProcessor() override
    {
        cloudSync.stopSync();
        // no parameter listeners registered
    }

    void parameterChanged (const juce::String& parameterID, float newValue) override
    {
        juce::ignoreUnused (parameterID, newValue);

    }


    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 1.0f, 0.0f, 1.0f), 0.35f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "predelay", 1 }, "Predelay", juce::NormalisableRange<float> (0.0f, 100.0f, 0.0f, 1.0f), 20.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f, 0.0f, 1.0f), 0.0f));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "bypass", 1 }, "Bypass", false));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "width", 1 }, "Width", juce::NormalisableRange<float> (0.0f, 1.0f, 0.0f, 0.85f), 0.35f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "decorrelate", 1 }, "Decorrelate", juce::NormalisableRange<float> (0.0f, 1.0f, 0.0f, 1.0f), 0.7f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = 1347570764u; // seed lock: reproducible renders




        rawParam_mix = apvts.getRawParameterValue ("mix");
        rawParam_predelay = apvts.getRawParameterValue ("predelay");
        rawParam_gain = apvts.getRawParameterValue ("gain");
        rawParam_bypass = apvts.getRawParameterValue ("bypass");
        rawParam_width = apvts.getRawParameterValue ("width");
        rawParam_decorrelate = apvts.getRawParameterValue ("decorrelate");
        sm_mix.reset (rawParam_mix != nullptr ? rawParam_mix->load() : 0.35f, 30.0f, sampleRate);
        sm_predelay.reset (rawParam_predelay != nullptr ? rawParam_predelay->load() : 20.0f, 25.0f, sampleRate);
        sm_gain.reset (rawParam_gain != nullptr ? rawParam_gain->load() : 0.0f, 30.0f, sampleRate);
        sm_bypass.reset (rawParam_bypass != nullptr ? rawParam_bypass->load() : 0.0f, 25.0f, sampleRate);
        sm_width.reset (rawParam_width != nullptr ? rawParam_width->load() : 0.35f, 20.0f, sampleRate);
        sm_decorrelate.reset (rawParam_decorrelate != nullptr ? rawParam_decorrelate->load() : 0.7f, 20.0f, sampleRate);

        // Mandatory 4× oversampling — every plugin, ignore Author features.oversampling.
        preparedSampleRate_ = sampleRate;
        preparedMaxBlockSize_ = samplesPerBlock;
        oversampling = std::make_unique<juce::dsp::Oversampling<float>> (2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        oversampling->initProcessing ((size_t) samplesPerBlock);
        int totalLatency = (int) std::ceil ((double) oversampling->getLatencyInSamples());

        reportedLatencySamples_ = juce::jmax (0, totalLatency);
        setLatencySamples (reportedLatencySamples_);
        updateHostDisplay (juce::AudioProcessorListener::ChangeDetails{}.withLatencyChanged (true));
        // Pre-size dry buffer with headroom so processBlock never grows on the audio thread.
        bypassDryBuf_.setSize (2, juce::jmax (samplesPerBlock, 2048), false, false, true);
        clicklessBypass_.prepare (sampleRate, samplesPerBlock, reportedLatencySamples_);
        const double dspSampleRate = sampleRate * (double) oversampling->getOversamplingFactor();
        const int dspSamplesPerBlock = samplesPerBlock * (int) oversampling->getOversamplingFactor();
        juce::dsp::ProcessSpec dspSpec { dspSampleRate, (juce::uint32) dspSamplesPerBlock, 2 };
        allpassDiffuserDsp_.prepare (dspSampleRate);
        phaseDecorrelatorDsp_.prepare (dspSampleRate);
        shimmerReverbDsp_.prepare (dspSampleRate);
    shimmerReverbDsp_.setParameters (
        0.55f,
        0.72f,
        0.45f,
        juce::jlimit (0.0f, 1.0f, smoothedParam ("mix")));
        for (auto& s : airShelfDsp_) s.prepare ((float) dspSampleRate);
        gainDsp.prepare (dspSpec); gainDsp.setRampDurationSeconds (0.02);
        signalSanityGuardDsp_.prepare (dspSampleRate);
        truePeakLeft.prepare ((float) sampleRate);
        truePeakRight.prepare ((float) sampleRate);
        analyzer.prepare (sampleRate);
        spectrumAnalyzer.prepare (sampleRate);

    }

    /** Host tempo helpers for custom DSP — always defined so JUCE compile never fails on unresolved symbols. */
    float getBpm() const
    {
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
                if (auto bpm = pos->getBpm())
                    return (float) *bpm;
        return 120.0f;
    }

    double getPlayHeadBeatPosition() const
    {
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
                if (auto ppq = pos->getPpqPosition())
                    return *ppq;
        return 0.0;
    }

    bool getPlayHeadIsPlaying() const
    {
        // JUCE 8 Position::getIsPlaying() returns bool (not Optional) — do not dereference.
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
                return pos->getIsPlaying();
        return false;
    }

    void releaseResources() override
    {
        if (oversampling != nullptr)
            oversampling->reset();
        oversampling.reset();
        truePeakLeft.reset();
        truePeakRight.reset();
        preparedMaxBlockSize_ = 0;
        preparedSampleRate_ = 0.0;
        reportedLatencySamples_ = 0;
        setLatencySamples (0);
        updateHostDisplay (juce::AudioProcessorListener::ChangeDetails{}.withLatencyChanged (true));
    }

    void reset() override
    {
        if (oversampling != nullptr)
            oversampling->reset();
        truePeakLeft.reset();
        truePeakRight.reset();
        if (rawParam_mix != nullptr)
            sm_mix.reset (rawParam_mix->load(), 30.0f, preparedSampleRate_ > 0.0 ? (float) preparedSampleRate_ : 48000.0f);
        if (rawParam_predelay != nullptr)
            sm_predelay.reset (rawParam_predelay->load(), 25.0f, preparedSampleRate_ > 0.0 ? (float) preparedSampleRate_ : 48000.0f);
        if (rawParam_gain != nullptr)
            sm_gain.reset (rawParam_gain->load(), 30.0f, preparedSampleRate_ > 0.0 ? (float) preparedSampleRate_ : 48000.0f);
        if (rawParam_bypass != nullptr)
            sm_bypass.reset (rawParam_bypass->load(), 25.0f, preparedSampleRate_ > 0.0 ? (float) preparedSampleRate_ : 48000.0f);
        if (rawParam_width != nullptr)
            sm_width.reset (rawParam_width->load(), 20.0f, preparedSampleRate_ > 0.0 ? (float) preparedSampleRate_ : 48000.0f);
        if (rawParam_decorrelate != nullptr)
            sm_decorrelate.reset (rawParam_decorrelate->load(), 20.0f, preparedSampleRate_ > 0.0 ? (float) preparedSampleRate_ : 48000.0f);
        phaseDecorrelatorDsp_.reset();
        if (preparedSampleRate_ > 0.0 && preparedMaxBlockSize_ > 0)
            clicklessBypass_.prepare (preparedSampleRate_, preparedMaxBlockSize_, reportedLatencySamples_);
    }

    bool isBusesLayoutSupported (const juce::AudioProcessor::BusesLayout& layouts) const override
    {
        // Effects: mono or stereo main I/O with matching channel counts (Logic AU requirement).
        const auto& mainIn  = layouts.getMainInputChannelSet();
        const auto& mainOut = layouts.getMainOutputChannelSet();
        if (mainOut != juce::AudioChannelSet::mono()
            && mainOut != juce::AudioChannelSet::stereo())
            return false;
        if (mainIn != mainOut)
            return false;

        return true;
    }
    void processChain (juce::dsp::AudioBlock<float> block)
    {
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        juce::ignoreUnused (ctx);
        // ---- built-in block: AllpassDiffuser (JUCE builtin) ----
    {
        auto* left = block.getChannelPointer (0);
        float* right = block.getNumChannels() > 1 ? block.getChannelPointer (1) : left;
        for (size_t i = 0; i < block.getNumSamples(); ++i)
            allpassDiffuserDsp_.process (left[i], right[i]);
    }
        // ---- built-in block: PhaseDecorrelator (JUCE builtin) ----
    {
        const float width = PhaseDecorrelator::clampWidth (smoothedParam ("width"));
        const float decorrelate = PhaseDecorrelator::clampDecorrelate (smoothedParam ("decorrelate"));
        auto* left = block.getChannelPointer (0);
        const bool stereoOut = block.getNumChannels() > 1;
        float* right = stereoOut ? block.getChannelPointer (1) : nullptr;
        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            float l = left[i];
            float r = stereoOut ? right[i] : l;
            phaseDecorrelatorDsp_.process (l, r, width, decorrelate);
            left[i] = l;
            if (stereoOut) right[i] = r;
        }
    }
        // ---- built-in block: ShimmerReverb (JUCE builtin) ----
    {
        shimmerReverbDsp_.setParameters (
            0.55f,
            0.72f,
            0.45f,
            juce::jlimit (0.0f, 1.0f, smoothedParam ("mix")));
        auto* left = block.getChannelPointer (0);
        float* right = block.getNumChannels() > 1 ? block.getChannelPointer (1) : left;
        for (size_t i = 0; i < block.getNumSamples(); ++i)
            shimmerReverbDsp_.processSample (left[i], right[i]);
    }
        // ---- built-in block: eq.air_shelf (JUCE builtin) ----
    {
        for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
        {
            auto& shelf = airShelfDsp_[juce::jmin ((size_t) 1, ch)];
            auto* d = block.getChannelPointer ((int) ch);
            for (size_t i = 0; i < block.getNumSamples(); ++i)
                d[i] = shelf.processSample (d[i]);
        }
    }
        // ---- built-in block: gain (JUCE builtin) ----
    gainDsp.setGainDecibels (smoothedParam ("gain"));
    gainDsp.process (ctx);
        // ---- built-in block: SignalSanityGuard (JUCE builtin) ----
    {
        for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
        {
            auto* data = block.getChannelPointer ((int) ch);
            for (size_t i = 0; i < block.getNumSamples(); ++i)
                data[i] = signalSanityGuardDsp_.sanitize (data[i]);
        }
    }
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ignoreUnused (midiMessages);
        juce::ScopedNoDenormals noDenormals;
        if (rawParam_mix != nullptr)
            sm_mix.setTarget (rawParam_mix->load());
        sm_mix.update (getSampleRate());
        if (rawParam_predelay != nullptr)
            sm_predelay.setTarget (rawParam_predelay->load());
        sm_predelay.update (getSampleRate());
        if (rawParam_gain != nullptr)
            sm_gain.setTarget (rawParam_gain->load());
        sm_gain.update (getSampleRate());
        if (rawParam_bypass != nullptr)
            sm_bypass.setTarget (rawParam_bypass->load());
        sm_bypass.update (getSampleRate());
        if (rawParam_width != nullptr)
            sm_width.setTarget (rawParam_width->load());
        sm_width.update (getSampleRate());
        if (rawParam_decorrelate != nullptr)
            sm_decorrelate.setTarget (rawParam_decorrelate->load());
        sm_decorrelate.update (getSampleRate());
        analyzer.pushBuffer (buffer);



        // Never allocate / re-prepare on the audio thread — AU hosts (Logic/auval) will crash.
        if (oversampling == nullptr || preparedSampleRate_ <= 0.0 || preparedMaxBlockSize_ <= 0)
        {
            buffer.clear();
            return;
        }
        if (buffer.getNumSamples() > preparedMaxBlockSize_
            || buffer.getNumChannels() > bypassDryBuf_.getNumChannels()
            || buffer.getNumSamples() > bypassDryBuf_.getNumSamples())
        {
            buffer.clear();
            return;
        }
        {
            const int n = buffer.getNumSamples();
            const int ch = buffer.getNumChannels();
            for (int c = 0; c < ch; ++c)
                bypassDryBuf_.copyFrom (c, 0, buffer, c, 0, n);
        }
        juce::dsp::AudioBlock<float> block (buffer);
        auto up = oversampling->processSamplesUp (block);
        processChain (up);
        oversampling->processSamplesDown (block);
        // ---- master output stage: -1 dB headroom trim + soft-knee limiter ----
        // Transparent below -6 dBFS; smoothly saturates the last 6 dB so the
        // chain can never hand the host a hard-clipped sample. Pure math,
        // zero allocation. (elite-audio / every generated plugin)
        {
            constexpr float trim = 0.891251f;      // -1 dB
            constexpr float knee = 0.501187f;      // -6 dBFS knee start
            constexpr float ceil_ = 0.985f;        // true-peak ceiling (-0.13 dB)
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                auto* d = buffer.getWritePointer (ch);
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                {
                    float x = d[i];
                    if (! std::isfinite (x))
                        x = 0.0f;
                    x *= trim;
                    const float ax = std::abs (x);
                    if (ax > knee)
                        x = (x > 0.0f ? 1.0f : -1.0f)
                            * (knee + (ceil_ - knee) * std::tanh ((ax - knee) / (ceil_ - knee)));
                    if (! std::isfinite (x))
                        x = 0.0f;
                    d[i] = x;
                }
            }
        }
        // Optional true-peak catch after soft-knee (legacy safety net)
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            TruePeakLimiter& lim = (ch == 0 ? truePeakLeft : truePeakRight);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                float y = lim.processSample (d[i]);
                d[i] = std::isfinite (y) ? y : 0.0f;
            }
        }


        {
            const bool wantBypass = (rawParam_bypass != nullptr && rawParam_bypass->load() >= 0.5f);
            clicklessBypass_.setBypassed (wantBypass);
            // Alias dry to the wet block length — bypassDryBuf_ is often oversized (2048).
            const int n = buffer.getNumSamples();
            const int ch = juce::jmin (buffer.getNumChannels(), bypassDryBuf_.getNumChannels());
            juce::AudioBuffer<float> dryView (bypassDryBuf_.getArrayOfWritePointers(), ch, n);
            clicklessBypass_.process (dryView, buffer);
            for (int c = 0; c < ch; ++c)
                buffer.copyFrom (c, 0, bypassDryBuf_, c, 0, n);
            // Final host-facing scrub: flush NaN/Inf (nan_recovery fixture) AND denormals.
            // A decaying reverb/delay tail copied straight to the output can carry subnormal
            // samples that ScopedNoDenormals / FTZ (which only flush arithmetic results, not
            // plain buffer copies) never catch — that trips the fixture's maxDenormal=0 gate
            // and spikes host CPU. 1e-15 (~-300 dBFS) is far below any audible signal and far
            // above the subnormal range, so it clears all denormals with no audible effect.
            for (int c = 0; c < buffer.getNumChannels(); ++c)
            {
                auto* d = buffer.getWritePointer (c);
                for (int i = 0; i < n; ++i)
                    if (! std::isfinite (d[i]) || std::abs (d[i]) < 1.0e-15f)
                        d[i] = 0.0f;
            }
        }
        analyzer.pushPostBuffer (buffer);
        {
            const int numCh = buffer.getNumChannels();
            const int numSamples = buffer.getNumSamples();
            if (numCh > 0 && numSamples > 0)
            {
                const float* L = buffer.getReadPointer (0);
                const float* R = numCh > 1 ? buffer.getReadPointer (1) : L;
                for (int i = 0; i < numSamples; ++i)
                    spectrumAnalyzer.pushSample (0.5f * (L[i] + R[i]));
            }
        }

        {
        double sumSq = 0.0;
        int sampleCount = 0;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const float* d = buffer.getReadPointer (ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                sumSq += (double) d[i] * (double) d[i];
                ++sampleCount;
            }
        }
        const float rms = sampleCount > 0 ? (float) std::sqrt (sumSq / (double) sampleCount) : 0.0f;
        outputRmsLevel.store (rms);
    }


    }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const noexcept override { return true; }
    const juce::String getName() const noexcept override { return "EchoChamber"; }
    bool acceptsMidi() const noexcept override { return false; }
    bool producesMidi() const noexcept override { return false; }

    bool isMidiEffect() const noexcept override { return false; }
    double getTailLengthSeconds() const noexcept override { return 4.0; }
    int getNumPrograms() noexcept override { return 11; }
    int getCurrentProgram() noexcept override { return currentProgram; }
    void setCurrentProgram (int index) override
    {
        currentProgram = juce::jlimit (0, getNumPrograms() - 1, index);
        applyPreset (currentProgram);
    }
    const juce::String getProgramName (int index) noexcept override
    {
        static const char* names[] = { "Init", "Vintage · Analog Push", "Vintage · Console Warm", "Vintage · Retro Room", "Vintage · Tape Bloom", "Vintage · Vintage Soft", "Modern · Modern Clean", "Modern · Pop Gloss", "Modern · Radio Ready", "Modern · Studio Edge", "Modern · Tight Bus" };
        return names[juce::jlimit (0, getNumPrograms() - 1, index)];
    }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        auto state = apvts.copyState();
        state.setProperty ("plugforgeVersion", ParameterVersioning::CURRENT_VERSION, nullptr);
        state.setProperty ("stateVersion", 1, nullptr);
        state.setProperty ("currentProgram", currentProgram, nullptr);
        juce::MemoryOutputStream stream (dest, false);
        state.writeToStream (stream);
    }
    void setStateInformation (const void* data, int size) override
    {
        auto tree = juce::ValueTree::readFromData (data, (size_t) size);
        if (! tree.isValid())
            return;
        const int savedVersion = (int) tree.getProperty ("stateVersion",
            (int) tree.getProperty ("plugforgeVersion", 1));
        auto vt = migrateState (tree, savedVersion);
        versionManager.applyMissingParameterDefaults (apvts, vt, savedVersion);
        apvts.replaceState (vt);
        currentProgram = (int) vt.getProperty ("currentProgram", currentProgram);
        // Do not call applyPreset here — that would overwrite recalled automation/state.
    }

    juce::AudioProcessorValueTreeState apvts;
    ParameterVersioning versionManager;
    WoManusAnalyzer analyzer;
    CloudSyncClient cloudSync;
    std::atomic<float> outputRmsLevel { 0.0f };
    SpectrumAnalyzer spectrumAnalyzer;


public:
    bool licensed = true;
    // Seed-locked deterministic RNG (xorshift32). All stochastic DSP must use
    // nextRandom() so identical input + identical automation = identical output.
    // Reset in prepareToPlay, so every render from the top is bit-reproducible.
    std::atomic<float>* rawParam_mix = nullptr;
    std::atomic<float>* rawParam_predelay = nullptr;
    std::atomic<float>* rawParam_gain = nullptr;
    std::atomic<float>* rawParam_bypass = nullptr;
    std::atomic<float>* rawParam_width = nullptr;
    std::atomic<float>* rawParam_decorrelate = nullptr;
    WoManusSmoothedParameter<float> sm_mix;
    WoManusSmoothedParameter<float> sm_predelay;
    WoManusSmoothedParameter<float> sm_gain;
    WoManusSmoothedParameter<float> sm_bypass;
    WoManusSmoothedParameter<float> sm_width;
    WoManusSmoothedParameter<float> sm_decorrelate;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    // Cached rawParam_* atomics are refreshed once per block in processBlock (smoothUpdate).
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "mix") == 0) return sm_mix.getNextValue();
        if (strcmp (id, "predelay") == 0) return sm_predelay.getNextValue();
        if (strcmp (id, "gain") == 0) return sm_gain.getNextValue();
        if (strcmp (id, "bypass") == 0) return sm_bypass.getNextValue();
        if (strcmp (id, "width") == 0) return sm_width.getNextValue();
        if (strcmp (id, "decorrelate") == 0) return sm_decorrelate.getNextValue();
        juce::ignoreUnused (id);
        return 0.0f;
    }

    /** Block-rate peek (no smoother advance) — use outside per-sample loops only. */
    inline float rawParamLoad (const char* id, float fallback = 0.0f) const noexcept
    {
        if (strcmp (id, "mix") == 0) return rawParam_mix != nullptr ? rawParam_mix->load() : fallback;
        if (strcmp (id, "predelay") == 0) return rawParam_predelay != nullptr ? rawParam_predelay->load() : fallback;
        if (strcmp (id, "gain") == 0) return rawParam_gain != nullptr ? rawParam_gain->load() : fallback;
        if (strcmp (id, "bypass") == 0) return rawParam_bypass != nullptr ? rawParam_bypass->load() : fallback;
        if (strcmp (id, "width") == 0) return rawParam_width != nullptr ? rawParam_width->load() : fallback;
        if (strcmp (id, "decorrelate") == 0) return rawParam_decorrelate != nullptr ? rawParam_decorrelate->load() : fallback;
        juce::ignoreUnused (id);
        return fallback;
    }





    uint32_t rngState_ = 1347570764u;
    inline float nextRandom() noexcept   // uniform [-1, 1)
    {
        rngState_ ^= rngState_ << 13; rngState_ ^= rngState_ >> 17; rngState_ ^= rngState_ << 5;
        return (float) (int32_t) rngState_ * 4.6566129e-10f;
    }
private:
    int currentProgram = 0;
    // State migration hook. Bump spec.stateVersion whenever a parameter is
    // added/renamed/rescaled, and translate old sessions here so customer
    // DAW projects never break. v1 is current.
    static juce::ValueTree migrateState (juce::ValueTree vt, int savedVersion)
    {
        if (! vt.isValid())
            return vt;

        // No explicit renames/rescales in spec.parameterMigrations — version stamp + defaults still apply.
        juce::ignoreUnused (savedVersion);

        vt.setProperty ("plugforgeVersion", ParameterVersioning::CURRENT_VERSION, nullptr);
        vt.setProperty ("stateVersion", 1, nullptr);
        return vt;
    }
    void applyPreset (int index)
    {
        switch (index)
        {
    case 0:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.35f));
        if (auto* param = apvts.getParameter ("predelay")) param->setValueNotifyingHost (param->convertTo0to1 (20.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.65f));
        if (auto* param = apvts.getParameter ("predelay")) param->setValueNotifyingHost (param->convertTo0to1 (45.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("predelay")) param->setValueNotifyingHost (param->convertTo0to1 (45.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.45f));
        if (auto* param = apvts.getParameter ("predelay")) param->setValueNotifyingHost (param->convertTo0to1 (75.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.55f));
        if (auto* param = apvts.getParameter ("predelay")) param->setValueNotifyingHost (param->convertTo0to1 (45.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4f));
        if (auto* param = apvts.getParameter ("predelay")) param->setValueNotifyingHost (param->convertTo0to1 (20.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.35f));
        if (auto* param = apvts.getParameter ("predelay")) param->setValueNotifyingHost (param->convertTo0to1 (20.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("predelay")) param->setValueNotifyingHost (param->convertTo0to1 (45.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("predelay")) param->setValueNotifyingHost (param->convertTo0to1 (20.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.6f));
        if (auto* param = apvts.getParameter ("predelay")) param->setValueNotifyingHost (param->convertTo0to1 (45.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.45f));
        if (auto* param = apvts.getParameter ("predelay")) param->setValueNotifyingHost (param->convertTo0to1 (45.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
        default: break;
        }


    }


    AllpassDiffuser allpassDiffuserDsp_;
    PhaseDecorrelator phaseDecorrelatorDsp_;
    TrustedShimmerReverb shimmerReverbDsp_;
    AirShelf airShelfDsp_[2];
    juce::dsp::Gain<float> gainDsp;
    SignalSanityGuard signalSanityGuardDsp_;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    ClicklessBypass clicklessBypass_;
    juce::AudioBuffer<float> bypassDryBuf_;
    int preparedMaxBlockSize_ = 0;
    double preparedSampleRate_ = 0.0;
    int reportedLatencySamples_ = 0;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    std::int64_t demoSampleCounter = 0;
    std::int64_t demoSilenceSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EchoChamberProcessor)
};
