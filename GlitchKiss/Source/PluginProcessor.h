#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <cstring>
#include <cmath>
#include "PlugForgeDenormals.h"
#include "WoManusParameterSmoothing.h"
#include "ParameterVersioning.h"





#include "WoManusAnalyzer.h"
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


class GlitchKissProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    GlitchKissProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, juce::Identifier ("PARAMS"), createLayout())
    {
        // Portable FTZ (x86 MXCSR / arm64 FPCR). processBlock also uses juce::ScopedNoDenormals.
        pfEnableFlushToZero();

        licensed = GlitchKissLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("GlitchKiss: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("mix", 1.0f, 1);
        versionManager.registerParameter ("bpm", 120.0f, 1);
        versionManager.registerParameter ("pattern1", 0.0f, 1);
        versionManager.registerParameter ("pattern2", 0.0f, 1);
        versionManager.registerParameter ("pattern3", 0.0f, 1);
        versionManager.registerParameter ("pattern4", 0.0f, 1);
        versionManager.registerParameter ("pattern5", 0.0f, 1);
        versionManager.registerParameter ("pattern6", 0.0f, 1);
        versionManager.registerParameter ("gain", 0.0f, 1);
        apvts.addParameterListener ("pattern1", this);
        apvts.addParameterListener ("pattern2", this);
        apvts.addParameterListener ("pattern3", this);
        apvts.addParameterListener ("pattern4", this);
        keyScaleCoeffsDirty.store (true);
    }

    ~GlitchKissProcessor() override
    {
        apvts.removeParameterListener ("pattern1", this);
        apvts.removeParameterListener ("pattern2", this);
        apvts.removeParameterListener ("pattern3", this);
        apvts.removeParameterListener ("pattern4", this);
    }

    void parameterChanged (const juce::String& parameterID, float newValue) override
    {
        juce::ignoreUnused (parameterID, newValue);
        if (parameterID == "pattern1" || parameterID == "pattern2" || parameterID == "pattern3" || parameterID == "pattern4")
            keyScaleCoeffsDirty.store (true);
    }

    void maybeRecalculateKeyScaleCoefficients()
    {
        if (! keyScaleCoeffsDirty.exchange (false))
            return;
        recalculateKeyScaleCoefficients();
    }

    void recalculateKeyScaleCoefficients()
    {
        // Key/Scale choice changes must refresh IIR / harmonic filter coefficients.
        // Never re-inject prepareToPlay snippets — only getSampleRate() is valid here.
        // No updateCoeffs — refuse prepareToPlay re-entry; use getSampleRate() only.
        juce::ignoreUnused (getSampleRate());
    }


    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "bpm", 1 }, "BPM", juce::NormalisableRange<float> (60.0f, 240.0f), 120.0f));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "pattern1", 1 }, "1/4 Note", false));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "pattern2", 1 }, "1/8 Note", false));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "pattern3", 1 }, "1/16 Note", false));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "pattern4", 1 }, "1/32 Note", false));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "pattern5", 1 }, "Triplet 1/8", false));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "pattern6", 1 }, "Dotted 1/8", false));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = 1347570764u; // seed lock: reproducible renders




        sm_mix.reset (apvts.getRawParameterValue ("mix")->load(), 5.0f, sampleRate);
        sm_bpm.reset (apvts.getRawParameterValue ("bpm")->load(), 10.0f, sampleRate);
        sm_pattern1.reset (apvts.getRawParameterValue ("pattern1")->load(), 10.0f, sampleRate);
        sm_pattern2.reset (apvts.getRawParameterValue ("pattern2")->load(), 10.0f, sampleRate);
        sm_pattern3.reset (apvts.getRawParameterValue ("pattern3")->load(), 10.0f, sampleRate);
        sm_pattern4.reset (apvts.getRawParameterValue ("pattern4")->load(), 10.0f, sampleRate);
        sm_pattern5.reset (apvts.getRawParameterValue ("pattern5")->load(), 10.0f, sampleRate);
        sm_pattern6.reset (apvts.getRawParameterValue ("pattern6")->load(), 10.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
            sampleRate = (float) sampleRate;
    // Initialize gate patterns (0=off, 1=on) for 1 beat (1/4 note) duration
    // 1/4 note (on for 1/4, off for 3/4)
    gatePatterns[0] = 0.25f; // On for 1/4 of a beat
    // 1/8 note (on for 1/8, off for 7/8)
    gatePatterns[1] = 0.125f; // On for 1/8 of a beat
    // 1/16 note (on for 1/16, off for 15/16)
    gatePatterns[2] = 0.0625f; // On for 1/16 of a beat
    // 1/32 note (on for 1/32, off for 31/32)
    gatePatterns[3] = 0.03125f; // On for 1/32 of a beat
    // Triplet 1/8 (on for 1/12, off for 11/12)
    gatePatterns[4] = 1.0f / 12.0f; // On for 1/12 of a beat
    // Dotted 1/8 (on for 3/16, off for 13/16)
    gatePatterns[5] = 3.0f / 16.0f; // On for 3/16 of a beat

    activePattern = 0;
    currentSampleCount = 0;
    lastMix = 0.0f;
    smoothedMix = 0.0f;

            gainDsp.prepare (dspSpec); gainDsp.setRampDurationSeconds (0.02);
        truePeakLeft.prepare ((float) sampleRate);
        truePeakRight.prepare ((float) sampleRate);
        analyzer.prepare (sampleRate);
    }
    void releaseResources() override
    {

        truePeakLeft.reset();
        truePeakRight.reset();
    }

    void processChain (juce::dsp::AudioBlock<float> block)
    {
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        juce::ignoreUnused (ctx);
        maybeRecalculateKeyScaleCoefficients();
        // ---- custom block: Glitch Kiss (AI-generated) ----
    // Signal flow: input → parameter read → algorithm → output
    const float wetDryMix = apvts.getRawParameterValue("mix")->load();
    const int pattern1 = (int)apvts.getRawParameterValue("pattern1")->load();
    const int pattern2 = (int)apvts.getRawParameterValue("pattern2")->load();
    const int pattern3 = (int)apvts.getRawParameterValue("pattern3")->load();
    const int pattern4 = (int)apvts.getRawParameterValue("pattern4")->load();
    const int pattern5 = (int)apvts.getRawParameterValue("pattern5")->load();
    const int pattern6 = (int)apvts.getRawParameterValue("pattern6")->load();
    const float bpm = apvts.getRawParameterValue("bpm")->load();

    // Determine active pattern (only one can be active)
    if (pattern1 == 1) activePattern = 0;
    else if (pattern2 == 1) activePattern = 1;
    else if (pattern3 == 1) activePattern = 2;
    else if (pattern4 == 1) activePattern = 3;
    else if (pattern5 == 1) activePattern = 4;
    else if (pattern6 == 1) activePattern = 5;

    const float samplesPerBeat = (60.0f / bpm) * sampleRate;
    const float gateOnFraction = gatePatterns[activePattern];
    const int gateOnSamples = (int)(gateOnFraction * samplesPerBeat);
    const int gateTotalSamples = (int)samplesPerBeat;

    // Smoothing for mix parameter
    smoothedMix = smoothedMix * 0.995f + wetDryMix * 0.005f; // 5ms smoothing

    for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
    {
        auto* channelData = block.getChannelPointer((int)ch);
        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            float in = channelData[i];
            float gatedSample = in;

            if (gateTotalSamples > 0) // Avoid division by zero
            {
                // Calculate current position within the beat cycle
                int beatPosition = currentSampleCount % gateTotalSamples;

                // Apply gate: on for the 'gateOnSamples' duration, off otherwise
                if (beatPosition >= gateOnSamples)
                {
                    gatedSample = 0.0f; // Gate is off
                }
            }

            // Apply wet/dry mix
            channelData[i] = in * (1.0f - smoothedMix) + gatedSample * smoothedMix;

            currentSampleCount++;
            if (currentSampleCount >= gateTotalSamples) // Reset counter at end of beat
                currentSampleCount = 0;
        }
    }
    // Self-check: mix→wet/dry blend; pattern1-6→activePattern→gateOnFraction→gatedSample; bpm→samplesPerBeat. All affect output.

        gainDsp.setGainDecibels (smoothedParam ("gain"));
    gainDsp.process (ctx);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ignoreUnused (midiMessages);
        juce::ScopedNoDenormals noDenormals;
        sm_mix.setTarget (apvts.getRawParameterValue ("mix")->load());
        sm_mix.update (getSampleRate());
        sm_bpm.setTarget (apvts.getRawParameterValue ("bpm")->load());
        sm_bpm.update (getSampleRate());
        sm_pattern1.setTarget (apvts.getRawParameterValue ("pattern1")->load());
        sm_pattern1.update (getSampleRate());
        sm_pattern2.setTarget (apvts.getRawParameterValue ("pattern2")->load());
        sm_pattern2.update (getSampleRate());
        sm_pattern3.setTarget (apvts.getRawParameterValue ("pattern3")->load());
        sm_pattern3.update (getSampleRate());
        sm_pattern4.setTarget (apvts.getRawParameterValue ("pattern4")->load());
        sm_pattern4.update (getSampleRate());
        sm_pattern5.setTarget (apvts.getRawParameterValue ("pattern5")->load());
        sm_pattern5.update (getSampleRate());
        sm_pattern6.setTarget (apvts.getRawParameterValue ("pattern6")->load());
        sm_pattern6.update (getSampleRate());
        sm_gain.setTarget (apvts.getRawParameterValue ("gain")->load());
        sm_gain.update (getSampleRate());
        analyzer.pushBuffer (buffer);



        processChain (juce::dsp::AudioBlock<float> (buffer));
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            TruePeakLimiter& lim = (ch == 0 ? truePeakLeft : truePeakRight);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                d[i] = lim.processSample (d[i]);
        }

        // DEMO MODE — remove when valid license key present
        demoSampleCounter++;
        if (demoSampleCounter > getSampleRate() * 45) {
            demoSilenceActive = true;
            demoSampleCounter = 0;
        }
        if (demoSilenceActive) {
            static int silenceCounter = 0;
            silenceCounter++;
            buffer.clear();
            if (silenceCounter > getSampleRate() * 3) {
                silenceCounter = 0;
                demoSilenceActive = false;
            }
        }
        analyzer.pushPostBuffer (buffer);
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
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "GlitchKiss"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }

    double getTailLengthSeconds() const override { return 2.0; }
    int getNumPrograms() override { return 14; }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override
    {
        currentProgram = juce::jlimit (0, getNumPrograms() - 1, index);
        applyPreset (currentProgram);
    }
    const juce::String getProgramName (int index) override
    {
        static const char* names[] = { "Init", "Daily Driver", "Studio Safe", "Gentle Push", "Mix Ready", "Clean Lift", "Techno", "Ambient", "Hip-Hop", "Cinematic", "Experimental", "Edge of Stability", "Total Commitment", "The Deep End" };
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
        versionManager.setStateInformation (apvts, data, size);
        if (auto tree = juce::ValueTree::readFromData (data, (size_t) size); tree.isValid())
        {
            const int savedVersion = (int) tree.getProperty ("stateVersion", 1);
            auto vt = migrateState (tree, savedVersion);
            apvts.replaceState (vt);
            currentProgram = (int) vt.getProperty ("currentProgram", 0);
            applyPreset (currentProgram);
        }
    }

    juce::AudioProcessorValueTreeState apvts;
    ParameterVersioning versionManager;
    WoManusAnalyzer analyzer;
    std::atomic<float> outputRmsLevel { 0.0f };

public:
    bool licensed = true;
    // Seed-locked deterministic RNG (xorshift32). All stochastic DSP must use
    // nextRandom() so identical input + identical automation = identical output.
    // Reset in prepareToPlay, so every render from the top is bit-reproducible.
    WoManusSmoothedParameter<float> sm_mix;
    WoManusSmoothedParameter<float> sm_bpm;
    WoManusSmoothedParameter<float> sm_pattern1;
    WoManusSmoothedParameter<float> sm_pattern2;
    WoManusSmoothedParameter<float> sm_pattern3;
    WoManusSmoothedParameter<float> sm_pattern4;
    WoManusSmoothedParameter<float> sm_pattern5;
    WoManusSmoothedParameter<float> sm_pattern6;
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "mix") == 0) return sm_mix.getNextValue();
        if (strcmp (id, "bpm") == 0) return sm_bpm.getNextValue();
        if (strcmp (id, "pattern1") == 0) return sm_pattern1.getNextValue();
        if (strcmp (id, "pattern2") == 0) return sm_pattern2.getNextValue();
        if (strcmp (id, "pattern3") == 0) return sm_pattern3.getNextValue();
        if (strcmp (id, "pattern4") == 0) return sm_pattern4.getNextValue();
        if (strcmp (id, "pattern5") == 0) return sm_pattern5.getNextValue();
        if (strcmp (id, "pattern6") == 0) return sm_pattern6.getNextValue();
        if (strcmp (id, "gain") == 0) return sm_gain.getNextValue();
        return 0.0f;
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
        juce::ignoreUnused (savedVersion);
        // if (savedVersion < 2) { /* e.g. vt.getChildWithProperty("id","oldName")... */ }
        return vt;
    }
    void applyPreset (int index)
    {
        switch (index)
        {
    case 0:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("bpm")) param->setValueNotifyingHost (param->convertTo0to1 (120.0f));
        if (auto* param = apvts.getParameter ("pattern1")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("pattern2")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("pattern3")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("pattern4")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("pattern5")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("pattern6")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("bpm")) param->setValueNotifyingHost (param->convertTo0to1 (137.5695f));
        if (auto* param = apvts.getParameter ("pattern1")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern2")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern3")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern4")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern5")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern6")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-1.6397f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("bpm")) param->setValueNotifyingHost (param->convertTo0to1 (129.4567f));
        if (auto* param = apvts.getParameter ("pattern1")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern2")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern3")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern4")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern5")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern6")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-2.6619f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("bpm")) param->setValueNotifyingHost (param->convertTo0to1 (119.0522f));
        if (auto* param = apvts.getParameter ("pattern1")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern2")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern3")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern4")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern5")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern6")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.0929f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("bpm")) param->setValueNotifyingHost (param->convertTo0to1 (117.1105f));
        if (auto* param = apvts.getParameter ("pattern1")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern2")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern3")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern4")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern5")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern6")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-0.1122f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("bpm")) param->setValueNotifyingHost (param->convertTo0to1 (122.2228f));
        if (auto* param = apvts.getParameter ("pattern1")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern2")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern3")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern4")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern5")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("pattern6")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (3.6828f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9235f));
        if (auto* param = apvts.getParameter ("bpm")) param->setValueNotifyingHost (param->convertTo0to1 (178.5636f));
        if (auto* param = apvts.getParameter ("pattern1")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("pattern2")) param->setValueNotifyingHost (param->convertTo0to1 (0.3194f));
        if (auto* param = apvts.getParameter ("pattern3")) param->setValueNotifyingHost (param->convertTo0to1 (0.6153f));
        if (auto* param = apvts.getParameter ("pattern4")) param->setValueNotifyingHost (param->convertTo0to1 (0.1522f));
        if (auto* param = apvts.getParameter ("pattern5")) param->setValueNotifyingHost (param->convertTo0to1 (0.1594f));
        if (auto* param = apvts.getParameter ("pattern6")) param->setValueNotifyingHost (param->convertTo0to1 (0.5144f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-7.3839f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.611f));
        if (auto* param = apvts.getParameter ("bpm")) param->setValueNotifyingHost (param->convertTo0to1 (124.8373f));
        if (auto* param = apvts.getParameter ("pattern1")) param->setValueNotifyingHost (param->convertTo0to1 (0.3435f));
        if (auto* param = apvts.getParameter ("pattern2")) param->setValueNotifyingHost (param->convertTo0to1 (0.4216f));
        if (auto* param = apvts.getParameter ("pattern3")) param->setValueNotifyingHost (param->convertTo0to1 (0.5097f));
        if (auto* param = apvts.getParameter ("pattern4")) param->setValueNotifyingHost (param->convertTo0to1 (0.4045f));
        if (auto* param = apvts.getParameter ("pattern5")) param->setValueNotifyingHost (param->convertTo0to1 (0.6807f));
        if (auto* param = apvts.getParameter ("pattern6")) param->setValueNotifyingHost (param->convertTo0to1 (0.607f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.9078f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4662f));
        if (auto* param = apvts.getParameter ("bpm")) param->setValueNotifyingHost (param->convertTo0to1 (117.0658f));
        if (auto* param = apvts.getParameter ("pattern1")) param->setValueNotifyingHost (param->convertTo0to1 (0.493f));
        if (auto* param = apvts.getParameter ("pattern2")) param->setValueNotifyingHost (param->convertTo0to1 (0.7102f));
        if (auto* param = apvts.getParameter ("pattern3")) param->setValueNotifyingHost (param->convertTo0to1 (0.7498f));
        if (auto* param = apvts.getParameter ("pattern4")) param->setValueNotifyingHost (param->convertTo0to1 (0.3233f));
        if (auto* param = apvts.getParameter ("pattern5")) param->setValueNotifyingHost (param->convertTo0to1 (0.4307f));
        if (auto* param = apvts.getParameter ("pattern6")) param->setValueNotifyingHost (param->convertTo0to1 (0.4871f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (16.0318f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5186f));
        if (auto* param = apvts.getParameter ("bpm")) param->setValueNotifyingHost (param->convertTo0to1 (183.519f));
        if (auto* param = apvts.getParameter ("pattern1")) param->setValueNotifyingHost (param->convertTo0to1 (0.4589f));
        if (auto* param = apvts.getParameter ("pattern2")) param->setValueNotifyingHost (param->convertTo0to1 (0.6105f));
        if (auto* param = apvts.getParameter ("pattern3")) param->setValueNotifyingHost (param->convertTo0to1 (0.6921f));
        if (auto* param = apvts.getParameter ("pattern4")) param->setValueNotifyingHost (param->convertTo0to1 (0.4644f));
        if (auto* param = apvts.getParameter ("pattern5")) param->setValueNotifyingHost (param->convertTo0to1 (0.7039f));
        if (auto* param = apvts.getParameter ("pattern6")) param->setValueNotifyingHost (param->convertTo0to1 (0.4435f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (11.4224f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.6904f));
        if (auto* param = apvts.getParameter ("bpm")) param->setValueNotifyingHost (param->convertTo0to1 (196.0936f));
        if (auto* param = apvts.getParameter ("pattern1")) param->setValueNotifyingHost (param->convertTo0to1 (0.1148f));
        if (auto* param = apvts.getParameter ("pattern2")) param->setValueNotifyingHost (param->convertTo0to1 (0.977f));
        if (auto* param = apvts.getParameter ("pattern3")) param->setValueNotifyingHost (param->convertTo0to1 (0.0247f));
        if (auto* param = apvts.getParameter ("pattern4")) param->setValueNotifyingHost (param->convertTo0to1 (0.1583f));
        if (auto* param = apvts.getParameter ("pattern5")) param->setValueNotifyingHost (param->convertTo0to1 (0.7612f));
        if (auto* param = apvts.getParameter ("pattern6")) param->setValueNotifyingHost (param->convertTo0to1 (0.7522f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (7.5401f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0165f));
        if (auto* param = apvts.getParameter ("bpm")) param->setValueNotifyingHost (param->convertTo0to1 (71.5188f));
        if (auto* param = apvts.getParameter ("pattern1")) param->setValueNotifyingHost (param->convertTo0to1 (0.0048f));
        if (auto* param = apvts.getParameter ("pattern2")) param->setValueNotifyingHost (param->convertTo0to1 (0.0588f));
        if (auto* param = apvts.getParameter ("pattern3")) param->setValueNotifyingHost (param->convertTo0to1 (0.0196f));
        if (auto* param = apvts.getParameter ("pattern4")) param->setValueNotifyingHost (param->convertTo0to1 (0.0379f));
        if (auto* param = apvts.getParameter ("pattern5")) param->setValueNotifyingHost (param->convertTo0to1 (0.0342f));
        if (auto* param = apvts.getParameter ("pattern6")) param->setValueNotifyingHost (param->convertTo0to1 (0.9915f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-21.7545f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0587f));
        if (auto* param = apvts.getParameter ("bpm")) param->setValueNotifyingHost (param->convertTo0to1 (233.2412f));
        if (auto* param = apvts.getParameter ("pattern1")) param->setValueNotifyingHost (param->convertTo0to1 (0.9307f));
        if (auto* param = apvts.getParameter ("pattern2")) param->setValueNotifyingHost (param->convertTo0to1 (0.993f));
        if (auto* param = apvts.getParameter ("pattern3")) param->setValueNotifyingHost (param->convertTo0to1 (0.0018f));
        if (auto* param = apvts.getParameter ("pattern4")) param->setValueNotifyingHost (param->convertTo0to1 (0.9976f));
        if (auto* param = apvts.getParameter ("pattern5")) param->setValueNotifyingHost (param->convertTo0to1 (0.9717f));
        if (auto* param = apvts.getParameter ("pattern6")) param->setValueNotifyingHost (param->convertTo0to1 (0.0392f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (20.8589f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9495f));
        if (auto* param = apvts.getParameter ("bpm")) param->setValueNotifyingHost (param->convertTo0to1 (230.2775f));
        if (auto* param = apvts.getParameter ("pattern1")) param->setValueNotifyingHost (param->convertTo0to1 (0.0775f));
        if (auto* param = apvts.getParameter ("pattern2")) param->setValueNotifyingHost (param->convertTo0to1 (0.9731f));
        if (auto* param = apvts.getParameter ("pattern3")) param->setValueNotifyingHost (param->convertTo0to1 (0.0074f));
        if (auto* param = apvts.getParameter ("pattern4")) param->setValueNotifyingHost (param->convertTo0to1 (0.0414f));
        if (auto* param = apvts.getParameter ("pattern5")) param->setValueNotifyingHost (param->convertTo0to1 (0.0463f));
        if (auto* param = apvts.getParameter ("pattern6")) param->setValueNotifyingHost (param->convertTo0to1 (0.9438f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (22.0294f));
        break;
        default: break;
        }


    }


    std::array<float, 6> gatePatterns;
    int activePattern = 0;
    int currentSampleCount = 0;
    float lastMix = 0.0f;
    float smoothedMix = 0.0f;
    float sampleRate = 44100.0f;

    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;
    std::atomic<bool> keyScaleCoeffsDirty { true };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlitchKissProcessor)
};
