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
        versionManager.registerParameter ("key", 0.0f, 1);
        versionManager.registerParameter ("scaleType", 0.0f, 1);
        versionManager.registerParameter ("masterTune", 440.0f, 1);
        versionManager.registerParameter ("harmonicStrength", 1.0f, 1);
        versionManager.registerParameter ("gain", 0.0f, 1);
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "key", 1 }, "Key", juce::NormalisableRange<float> (0.0f, 11.0f), 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "scaleType", 1 }, "Scale Type", juce::NormalisableRange<float> (0.0f, 6.0f), 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "masterTune", 1 }, "Master Tune", juce::NormalisableRange<float> (432.0f, 444.0f), 440.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "harmonicStrength", 1 }, "Harmonic Strength", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = ${SEED}u; // seed lock: reproducible renders




        sm_key.init (apvts.getRawParameterValue ("key")->load(), 20.0f, (float) sampleRate);
        sm_scaleType.init (apvts.getRawParameterValue ("scaleType")->load(), 20.0f, (float) sampleRate);
        sm_masterTune.init (apvts.getRawParameterValue ("masterTune")->load(), 20.0f, (float) sampleRate);
        sm_harmonicStrength.init (apvts.getRawParameterValue ("harmonicStrength")->load(), 20.0f, (float) sampleRate);
        sm_gain.init (apvts.getRawParameterValue ("gain")->load(), 20.0f, (float) sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
            for (int i = 0; i < maxNumFilters; ++i)
    {
        filters[i].reset();
        coefficients[i] = juce::dsp::IIR::Coefficients<float>::makePeakFilter(44100.0f, 1000.0f, 1.0f, 1.0f);
        filters[i].setCoefficients(*coefficients[i]);
    }
    lastSampleRate = 0.0f;
    smoothedMasterTune = 440.0f;
    smoothedHarmonicStrength = 0.0f;
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
    const float smoothingCoeff = 0.005f;

    // Parameter smoothing
    smoothedMasterTune += smoothingCoeff * (masterTune - smoothedMasterTune);
    smoothedHarmonicStrength += smoothingCoeff * (harmonicStrength - smoothedHarmonicStrength);

    // Check if filters need updating
    if (sampleRate != lastSampleRate || std::abs(masterTune - smoothedMasterTune) > 0.01f || std::abs(harmonicStrength - smoothedHarmonicStrength) > 0.01f)
    {
        updateFilters(sampleRate);
        lastSampleRate = sampleRate;
    }

    for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
    {
        auto* d = block.getChannelPointer ((int) ch);
        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            float sample = d[i];
            for (int f = 0; f < maxNumFilters; ++f)
            {
                if (coefficients[f] != nullptr) // Only process active filters
                {
                    sample = filters[f].processSample(sample);
                }
            }
            d[i] = sample;
        }
    }
        gainDsp.setGainDecibels (smoothedParam ("gain"));
    gainDsp.process (ctx);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ScopedNoDenormals noDenormals;
        sm_key.setTarget (apvts.getRawParameterValue ("key")->load());
        sm_scaleType.setTarget (apvts.getRawParameterValue ("scaleType")->load());
        sm_masterTune.setTarget (apvts.getRawParameterValue ("masterTune")->load());
        sm_harmonicStrength.setTarget (apvts.getRawParameterValue ("harmonicStrength")->load());
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
    ParameterSmoother sm_key;
    ParameterSmoother sm_scaleType;
    ParameterSmoother sm_masterTune;
    ParameterSmoother sm_harmonicStrength;
    ParameterSmoother sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one control-block step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "key") == 0) return sm_key.nextValue();
        if (strcmp (id, "scaleType") == 0) return sm_scaleType.nextValue();
        if (strcmp (id, "masterTune") == 0) return sm_masterTune.nextValue();
        if (strcmp (id, "harmonicStrength") == 0) return sm_harmonicStrength.nextValue();
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
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (440.0f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (439.6055f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.0076f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (439.7708f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-2.1931f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (439.0821f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (4.5497f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (439.7044f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.8492f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (439.3732f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-0.5516f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (6.597f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (5.0341f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (437.0321f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (0.8947f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-11.4959f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (6.7247f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.4792f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (436.1953f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (0.1035f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (4.1286f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (2.6214f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (0.8441f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (437.8457f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (0.7591f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (16.3f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (7.2807f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (4.5011f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (435.9014f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (0.7341f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-8.5409f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (7.9308f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.3824f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (432.4925f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (0.7238f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-15.7168f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (10.687f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (0.3651f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (432.4864f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (0.0792f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (22.4841f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (0.2216f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (0.1999f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (443.945f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (0.951f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-22.5253f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (0.7423f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (5.9522f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (432.1878f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (0.0279f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-22.6567f));
        break;
        default: break;
        }


    }


    static constexpr int maxNumFilters = 24;
    std::array<juce::dsp::IIR::Filter<float>, maxNumFilters> filters;
    std::array<juce::dsp::IIR::Coefficients<float>::Ptr, maxNumFilters> coefficients;

    float lastSampleRate = 0.0f;

    // Parameters
    int currentKey = 0; // C=0, C#=1, ..., B=11
    int currentScaleType = 0; // Major=0, Natural Minor=1, ..., Phrygian Dominant=6
    float masterTune = 440.0f;
    float harmonicStrength = 0.0f; // 0-1.0

    // Internal state for smoothing
    float smoothedMasterTune = 440.0f;
    float smoothedHarmonicStrength = 0.0f;

    // Scale definitions (MIDI note offsets from root)
    // Major, Natural Minor, Dorian, Phrygian, Lydian, Mixolydian, Phrygian Dominant
    const std::array<std::vector<int>, 7> scaleIntervals = {
        std::vector<int>{0, 2, 4, 5, 7, 9, 11}, // Major
        std::vector<int>{0, 2, 3, 5, 7, 8, 10}, // Natural Minor
        std::vector<int>{0, 2, 3, 5, 7, 9, 10}, // Dorian
        std::vector<int>{0, 1, 3, 5, 7, 8, 10}, // Phrygian
        std::vector<int>{0, 2, 4, 6, 7, 9, 11}, // Lydian
        std::vector<int>{0, 2, 4, 5, 7, 9, 10}, // Mixolydian
        std::vector<int>{0, 1, 4, 5, 7, 8, 10}  // Phrygian Dominant (harmonic minor with b2)
    };

    // Helper to calculate frequency from MIDI note number
    float midiNoteToFrequency(int midiNote, float tune) const
    {
        return tune * std::pow(2.0f, (midiNote - 69) / 12.0f);
    }

    void updateFilters(float sampleRate)
    {
        // Clear existing filters
        for (int i = 0; i < maxNumFilters; ++i)
        {
            coefficients[i] = juce::dsp::IIR::Coefficients<float>::Ptr(nullptr);
            filters[i].reset();
        }

        const auto& currentScale = scaleIntervals[currentScaleType];
        std::vector<int> inKeyMidiNotes;
        std::vector<int> outOfKeyMidiNotes;

        // Populate in-key and out-of-key MIDI notes within the 20Hz-20kHz range
        for (int octave = 0; octave < 10; ++octave) // Covers MIDI notes from ~24 to ~120
        {
            for (int i = 0; i < 12; ++i)
            {
                int midiNote = 12 * (octave + 1) + currentKey + i; // Start from C1 (MIDI 24)
                float freq = midiNoteToFrequency(midiNote, smoothedMasterTune);

                if (freq >= 20.0f && freq <= 20000.0f)
                {
                    bool isInKey = false;
                    for (int interval : currentScale)
                    {
                        if (i == interval) // Check if the note within the octave is in the scale
                        {
                            isInKey = true;
                            break;
                        }
                    }

                    if (isInKey)
                        inKeyMidiNotes.push_back(midiNote);
                    else
                        outOfKeyMidiNotes.push_back(midiNote);
                }
            }
        }

        int filterIdx = 0;

        // Add in-key boost filters
        for (int midiNote : inKeyMidiNotes)
        {
            if (filterIdx >= maxNumFilters) break;
            float freq = midiNoteToFrequency(midiNote, smoothedMasterTune);
            if (freq < 20.0f || freq > 20000.0f) continue;

            coefficients[filterIdx] = juce::dsp::IIR::Coefficients<float>::makePeakFilter(
                sampleRate, freq, 2.0f, 1.0f + 1.5f * smoothedHarmonicStrength
            );
            filters[filterIdx].setCoefficients(*coefficients[filterIdx]);
            filterIdx++;
        }

        // Add out-of-key cut filters
        for (int midiNote : outOfKeyMidiNotes)
        {
            if (filterIdx >= maxNumFilters) break;
            float freq = midiNoteToFrequency(midiNote, smoothedMasterTune);
            if (freq < 20.0f || freq > 20000.0f) continue;

            coefficients[filterIdx] = juce::dsp::IIR::Coefficients<float>::makePeakFilter(
                sampleRate, freq, 1.5f, 1.0f - 0.8f * smoothedHarmonicStrength
            );
            filters[filterIdx].setCoefficients(*coefficients[filterIdx]);
            filterIdx++;
        }
    }
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Harmonic2KProcessor)
};
