#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <cstring>
#include <cmath>
#include <xmmintrin.h>
#include "ParameterSmoother.h"
#include "ParameterVersioning.h"





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


class Harmonic2KProcessor : public juce::AudioProcessor
{
public:
    Harmonic2KProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "PARAMS", createLayout())
    {
        _mm_setcsr (_mm_getcsr() | 0x8040);

        licensed = Harmonic2KLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("Harmonic2K: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("strength", 50.0f, 1);
        versionManager.registerParameter ("tune", 440.0f, 1);
        versionManager.registerParameter ("rootNote", 0.0f, 1);
        versionManager.registerParameter ("scaleType", 0.0f, 1);
        versionManager.registerParameter ("gain", 0.0f, 1);
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "strength", 1 }, "Strength", juce::NormalisableRange<float> (0.0f, 100.0f), 50.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "tune", 1 }, "Master Tune", juce::NormalisableRange<float> (432.0f, 444.0f), 440.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "rootNote", 1 }, "Root Note", juce::NormalisableRange<float> (0.0f, 11.0f), 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "scaleType", 1 }, "Scale Type", juce::NormalisableRange<float> (0.0f, 6.0f), 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = ${SEED}u; // seed lock: reproducible renders




        sm_strength.init (apvts.getRawParameterValue ("strength")->load(), 20.0f, (float) sampleRate);
        sm_tune.init (apvts.getRawParameterValue ("tune")->load(), 20.0f, (float) sampleRate);
        sm_rootNote.init (apvts.getRawParameterValue ("rootNote")->load(), 20.0f, (float) sampleRate);
        sm_scaleType.init (apvts.getRawParameterValue ("scaleType")->load(), 20.0f, (float) sampleRate);
        sm_gain.init (apvts.getRawParameterValue ("gain")->load(), 20.0f, (float) sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
            for (int i = 0; i < maxNumFilters; ++i)
    {
        filters[i].reset();
        currentGains[i] = 1.0f;
        currentQs[i] = 1.0f;
        filterActive[i] = false;
    }
    smoothStrength = 0.0f;
    smoothTune = 440.0f;

    calculateFilterParams(sampleRate, 0, 0, 0.0f, 440.0f); // Initial calculation for C Major, 0 strength, A4=440Hz
            gainDsp.prepare (dspSpec); gainDsp.setRampDurationSeconds (0.02);
        truePeakLeft.prepare ((float) sampleRate);
        truePeakRight.prepare ((float) sampleRate);
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
        // ---- custom block: Harmonic2K (AI-generated) ----
    const float sampleRate = (float) getSampleRate();
    const float strength = apvts.getRawParameterValue("strength")->load() / 100.0f;
    const float tune = apvts.getRawParameterValue("tune")->load();
    const int rootNote = static_cast<int>(apvts.getRawParameterValue("rootNote")->load());
    const int scaleType = static_cast<int>(apvts.getRawParameterValue("scaleType")->load());

    const float smoothingCoeff = 0.01f; // One-pole smoothing

    smoothStrength += smoothingCoeff * (strength - smoothStrength);
    smoothTune += smoothingCoeff * (tune - smoothTune);

    // Recalculate filter parameters if any relevant parameter changes
    // This is a simplification; in a real plugin, you'd check if parameters actually changed
    // or use a flag to trigger recalculation less frequently.
    // For this example, we'll recalculate every block if strength/tune/root/scale are different
    // from the last smoothed values. This might be too frequent for real-time.
    // A more robust solution would be to use a `std::atomic<bool> parametersChanged` flag
    // set by the APVTS listeners and reset after recalculation.
    static float lastStrength = -1.0f, lastTune = -1.0f, lastRootNote = -1.0f, lastScaleType = -1.0f;
    if (std::abs(strength - lastStrength) > 0.001f || std::abs(tune - lastTune) > 0.001f ||
        rootNote != static_cast<int>(lastRootNote) || scaleType != static_cast<int>(lastScaleType))
    {
        calculateFilterParams(sampleRate, rootNote, scaleType, smoothStrength, smoothTune);
        lastStrength = strength;
        lastTune = tune;
        lastRootNote = static_cast<float>(rootNote);
        lastScaleType = static_cast<float>(scaleType);
    }

    for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
    {
        auto* d = block.getChannelPointer((int) ch);
        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            float currentSample = d[i];
            for (int filterIdx = 0; filterIdx < numActiveFilters; ++filterIdx)
            {
                if (filterActive[filterIdx])
                {
                    currentGains[filterIdx] += smoothingCoeff * (targetGains[filterIdx] - currentGains[filterIdx]);
                    currentQs[filterIdx] += smoothingCoeff * (targetQs[filterIdx] - currentQs[filterIdx]);

                    // Update filter coefficients smoothly
                    *filters[filterIdx].coefficients = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(
                        sampleRate, targetFrequencies[filterIdx], currentQs[filterIdx], currentGains[filterIdx]);

                    currentSample = filters[filterIdx].processSample(currentSample);
                }
            }
            d[i] = currentSample;
        }
    }
        gainDsp.setGainDecibels (smoothedParam ("gain"));
    gainDsp.process (ctx);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ScopedNoDenormals noDenormals;
        sm_strength.setTarget (apvts.getRawParameterValue ("strength")->load());
        sm_tune.setTarget (apvts.getRawParameterValue ("tune")->load());
        sm_rootNote.setTarget (apvts.getRawParameterValue ("rootNote")->load());
        sm_scaleType.setTarget (apvts.getRawParameterValue ("scaleType")->load());
        sm_gain.setTarget (apvts.getRawParameterValue ("gain")->load());



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
    const juce::String getName() const override { return "Harmonic2K"; }
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
    std::atomic<float> outputRmsLevel { 0.0f };

public:
    bool licensed = true;
    // Seed-locked deterministic RNG (xorshift32). All stochastic DSP must use
    // nextRandom() so identical input + identical automation = identical output.
    // Reset in prepareToPlay, so every render from the top is bit-reproducible.
    ParameterSmoother sm_strength;
    ParameterSmoother sm_tune;
    ParameterSmoother sm_rootNote;
    ParameterSmoother sm_scaleType;
    ParameterSmoother sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one control-block step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "strength") == 0) return sm_strength.nextValue();
        if (strcmp (id, "tune") == 0) return sm_tune.nextValue();
        if (strcmp (id, "rootNote") == 0) return sm_rootNote.nextValue();
        if (strcmp (id, "scaleType") == 0) return sm_scaleType.nextValue();
        if (strcmp (id, "gain") == 0) return sm_gain.nextValue();
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
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (50.0f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (440.0f));
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (48.8075f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (440.4f));
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.0076f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (42.6627f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (440.4f));
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-2.1931f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (51.4246f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (440.1578f));
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (4.5497f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (40.3269f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (438.8806f));
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.8492f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (55.1238f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (439.255f));
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-0.5516f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (59.9724f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (442.0682f));
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (4.6127f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (5.3682f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-11.4959f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (61.134f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (434.9585f));
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (3.8457f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (0.6209f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (4.1286f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (23.8309f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (433.6883f));
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (5.3585f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (4.5549f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (16.3f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (66.1882f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (441.0021f));
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (3.5762f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (4.4047f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-8.5409f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (72.0983f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (434.7649f));
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (0.4515f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (4.3427f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-15.7168f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (97.1542f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (432.7303f));
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (0.4459f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (0.4753f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (22.4841f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (2.0145f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (432.3997f));
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (10.9496f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (5.7062f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-22.5253f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (6.7482f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (443.9043f));
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (0.1721f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (0.1673f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-22.6567f));
        break;
        default: break;
        }


    }


    static constexpr int maxNumFilters = 24;
    std::array<juce::dsp::IIR::Filter<float>, maxNumFilters> filters;
    std::array<float, maxNumFilters> targetFrequencies;
    std::array<float, maxNumFilters> currentGains;
    std::array<float, maxNumFilters> targetGains;
    std::array<float, maxNumFilters> currentQs;
    std::array<float, maxNumFilters> targetQs;
    std::array<bool, maxNumFilters> filterActive;
    int numActiveFilters = 0;

    float smoothStrength = 0.0f;
    float smoothTune = 440.0f;

    // Lookup tables for scale degrees and MIDI notes
    // Major: 0, 2, 4, 5, 7, 9, 11
    // Natural Minor: 0, 2, 3, 5, 7, 8, 10
    // Dorian: 0, 2, 3, 5, 7, 9, 10
    // Phrygian: 0, 1, 3, 5, 7, 8, 10
    // Lydian: 0, 2, 4, 6, 7, 9, 11
    // Mixolydian: 0, 2, 4, 5, 7, 9, 10
    // Phrygian Dominant: 0, 1, 4, 5, 7, 8, 10
    std::array<std::array<int, 7>, 7> scaleIntervals = {{
        {{0, 2, 4, 5, 7, 9, 11}},  // Major
        {{0, 2, 3, 5, 7, 8, 10}},  // Natural Minor
        {{0, 2, 3, 5, 7, 9, 10}},  // Dorian
        {{0, 1, 3, 5, 7, 8, 10}},  // Phrygian
        {{0, 2, 4, 6, 7, 9, 11}},  // Lydian
        {{0, 2, 4, 5, 7, 9, 10}},  // Mixolydian
        {{0, 1, 4, 5, 7, 8, 10}}   // Phrygian Dominant
    }};

    void calculateFilterParams(float sampleRate, int rootNote, int scaleType, float strength, float tune)
    {
        for (int i = 0; i < maxNumFilters; ++i)
        {
            filterActive[i] = false;
        }
        numActiveFilters = 0;

        // MIDI notes for C0 to B8 (0-107)
        // We care about 20Hz to 20kHz, which is roughly MIDI 24 (C1) to MIDI 108 (C9)
        // Let's iterate through MIDI notes from 24 to 108

        std::array<bool, 12> inKeyDegrees;
        for (int i = 0; i < 12; ++i) inKeyDegrees[i] = false;
        for (int i = 0; i < 7; ++i)
        {
            inKeyDegrees[scaleIntervals[scaleType][i]] = true;
        }

        // Iterate through octaves and notes to find in-key and out-of-key frequencies
        for (int midiNote = 24; midiNote <= 108; ++midiNote)
        {
            float freq = tune * std::pow(2.0f, (midiNote - 69) / 12.0f);

            if (freq < 20.0f || freq > 20000.0f) continue;

            int noteDegree = (midiNote - rootNote + 120) % 12;

            if (numActiveFilters < maxNumFilters)
            {
                targetFrequencies[numActiveFilters] = freq;

                if (inKeyDegrees[noteDegree])
                {
                    targetGains[numActiveFilters] = 1.0f + (1.5f / 3.0f) * strength; // +1.5dB boost
                    targetQs[numActiveFilters] = 2.0f;
                }
                else
                {
                    targetGains[numActiveFilters] = 1.0f - (0.8f / 3.0f) * strength; // -0.8dB cut
                    targetQs[numActiveFilters] = 1.5f;
                }
                filterActive[numActiveFilters] = true;
                numActiveFilters++;
            }
        }

        // If we have more than maxNumFilters, we'll just use the first maxNumFilters found.
        // This prioritizes lower frequencies if we hit the limit early.
    }
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Harmonic2KProcessor)
};
