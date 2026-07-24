#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <cstring>
#include <cmath>
#include <xmmintrin.h>
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


class Harmonic2KProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
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
        versionManager.registerParameter ("rootNote", 0.0f, 1);
        versionManager.registerParameter ("scaleType", 0.0f, 1);
        versionManager.registerParameter ("masterTune", 440.0f, 1);
        versionManager.registerParameter ("harmonicStrength", 100.0f, 1);
        versionManager.registerParameter ("gain", 0.0f, 1);
        apvts.addParameterListener ("rootNote", this);
        apvts.addParameterListener ("scaleType", this);
        keyScaleCoeffsDirty.store (true);
    }

    ~Harmonic2KProcessor() override
    {
        apvts.removeParameterListener ("rootNote", this);
        apvts.removeParameterListener ("scaleType", this);
    }

    void parameterChanged (const juce::String& parameterID, float newValue) override
    {
        juce::ignoreUnused (newValue);
        if (parameterID == "rootNote" || parameterID == "scaleType")
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
        // Custom blocks without updateCoeffs: re-run prepare snippets that touch coefficients.
        currentSampleRate = (float) getSampleRate();
for (auto& filter : filters)
    filter.reset();

// Initialize filter coefficients to bypass to avoid NaNs before first update
for (int i = 0; i < maxActiveFilters; ++i)
{
    filterCoefficients[i] = juce::dsp::IIR::Coefficients<float>::makePeakFilter(currentSampleRate, 1000.0f, 0.707f, 1.0f);
    filters[i].setCoefficients(filterCoefficients[i]);
}

// Initialize pre-allocated UI message objects
eqDataMessage = juce::DynamicObject::fromJSON("{}");
eqDataMessage.getDynamicObject()->setProperty("type", "eqBandsUpdate");
pianoDataMessage = juce::DynamicObject::fromJSON("{}");
pianoDataMessage.getDynamicObject()->setProperty("type", "pianoUpdate");

updateFilterCoefficients();
            gainDsp.prepare (dspSpec); gainDsp.setRampDurationSeconds (0.02);
    }


    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "rootNote", 1 }, "Key", juce::StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0));
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "scaleType", 1 }, "Scale", juce::StringArray { "Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Phrygian Dom" }, 0));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "masterTune", 1 }, "Tune", juce::NormalisableRange<float> (432.0f, 444.0f), 440.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "harmonicStrength", 1 }, "Strength", juce::NormalisableRange<float> (0.0f, 100.0f), 100.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = ${SEED}u; // seed lock: reproducible renders




        sm_rootNote.reset (apvts.getRawParameterValue ("rootNote")->load(), 10.0f, sampleRate);
        sm_scaleType.reset (apvts.getRawParameterValue ("scaleType")->load(), 10.0f, sampleRate);
        sm_masterTune.reset (apvts.getRawParameterValue ("masterTune")->load(), 10.0f, sampleRate);
        sm_harmonicStrength.reset (apvts.getRawParameterValue ("harmonicStrength")->load(), 10.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        currentSampleRate = (float) getSampleRate();
for (auto& filter : filters)
    filter.reset();

// Initialize filter coefficients to bypass to avoid NaNs before first update
for (int i = 0; i < maxActiveFilters; ++i)
{
    filterCoefficients[i] = juce::dsp::IIR::Coefficients<float>::makePeakFilter(currentSampleRate, 1000.0f, 0.707f, 1.0f);
    filters[i].setCoefficients(filterCoefficients[i]);
}

// Initialize pre-allocated UI message objects
eqDataMessage = juce::DynamicObject::fromJSON("{}");
eqDataMessage.getDynamicObject()->setProperty("type", "eqBandsUpdate");
pianoDataMessage = juce::DynamicObject::fromJSON("{}");
pianoDataMessage.getDynamicObject()->setProperty("type", "pianoUpdate");

updateFilterCoefficients();
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
        // ---- custom block: Harmonic2K (AI-generated) ----
if (sm_rootNote != (int)smoothedParam("rootNote").skip(1) ||
    sm_scaleType != (int)smoothedParam("scaleType").skip(1) ||
    sm_masterTune != smoothedParam("masterTune").skip(1) ||
    sm_harmonicStrength != smoothedParam("harmonicStrength").skip(1))
{
    sm_rootNote = (int)smoothedParam("rootNote").skip(1);
    sm_scaleType = (int)smoothedParam("scaleType").skip(1);
    sm_masterTune = smoothedParam("masterTune").skip(1);
    sm_harmonicStrength = smoothedParam("harmonicStrength").skip(1);
    updateFilterCoefficients();
}

// UI update logic - moved to a separate flag to ensure data is prepared once
if (needsUiUpdate)
{
    eqFrequenciesArray.clear();
    eqGainsArray.clear();
    for(int i = 0; i < uiActiveFilterCount; ++i)
    {
        eqFrequenciesArray.add(uiFreqs[i]);
        eqGainsArray.add(uiGains[i]);
    }
    eqDataMessage.getDynamicObject()->setProperty("frequencies", eqFrequenciesArray);
    eqDataMessage.getDynamicObject()->setProperty("gains", eqGainsArray);
    postMessage(eqDataMessage);

    pianoInKeyNotesArray.clear();
    for(int i = 0; i < uiInKeyNotesCount; ++i)
    {
        pianoInKeyNotesArray.add(uiInKeyMidiNotes[i]);
    }
    pianoDataMessage.getDynamicObject()->setProperty("inKeyMidiNotes", pianoInKeyNotesArray);
    postMessage(pianoDataMessage);

    needsUiUpdate = false;
}

for (size_t ch = 0; ch < block.getNumChannels(); ++ch) {
    auto* d = block.getChannelPointer ((int) ch);
    for (size_t i = 0; i < block.getNumSamples(); ++i) {
        float sample = d[i];
        for (int f = 0; f < maxActiveFilters; ++f) {
            sample = filters[f].processSample(sample);
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
        sm_rootNote.setTarget (apvts.getRawParameterValue ("rootNote")->load());
        sm_rootNote.update (getSampleRate());
        sm_scaleType.setTarget (apvts.getRawParameterValue ("scaleType")->load());
        sm_scaleType.update (getSampleRate());
        sm_masterTune.setTarget (apvts.getRawParameterValue ("masterTune")->load());
        sm_masterTune.update (getSampleRate());
        sm_harmonicStrength.setTarget (apvts.getRawParameterValue ("harmonicStrength")->load());
        sm_harmonicStrength.update (getSampleRate());
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
    WoManusAnalyzer analyzer;
    std::atomic<float> outputRmsLevel { 0.0f };

public:
    bool licensed = true;
    // Seed-locked deterministic RNG (xorshift32). All stochastic DSP must use
    // nextRandom() so identical input + identical automation = identical output.
    // Reset in prepareToPlay, so every render from the top is bit-reproducible.
    WoManusSmoothedParameter<float> sm_rootNote;
    WoManusSmoothedParameter<float> sm_scaleType;
    WoManusSmoothedParameter<float> sm_masterTune;
    WoManusSmoothedParameter<float> sm_harmonicStrength;
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "rootNote") == 0) return sm_rootNote.getNextValue();
        if (strcmp (id, "scaleType") == 0) return sm_scaleType.getNextValue();
        if (strcmp (id, "masterTune") == 0) return sm_masterTune.getNextValue();
        if (strcmp (id, "harmonicStrength") == 0) return sm_harmonicStrength.getNextValue();
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
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (440.0f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (100.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (439.6055f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.0076f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (439.7708f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-2.1931f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (439.0821f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (4.5497f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (439.7044f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.8492f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (439.3732f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-0.5516f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (6.597f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (5.0341f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (437.0321f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (89.4697f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-11.4959f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (6.7247f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.4792f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (436.1953f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (10.3478f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (4.1286f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (2.6214f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (0.8441f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (437.8457f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (75.9149f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (16.3f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (7.2807f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (4.5011f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (435.9014f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (73.4119f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-8.5409f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (7.9308f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (1.3824f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (432.4925f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (72.3782f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-15.7168f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (10.687f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (0.3651f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (432.4864f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (7.9213f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (22.4841f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (0.2216f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (0.1999f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (443.945f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (95.104f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-22.5253f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("rootNote")) param->setValueNotifyingHost (param->convertTo0to1 (0.7423f));
        if (auto* param = apvts.getParameter ("scaleType")) param->setValueNotifyingHost (param->convertTo0to1 (5.9522f));
        if (auto* param = apvts.getParameter ("masterTune")) param->setValueNotifyingHost (param->convertTo0to1 (432.1878f));
        if (auto* param = apvts.getParameter ("harmonicStrength")) param->setValueNotifyingHost (param->convertTo0to1 (2.789f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-22.6567f));
        break;
        default: break;
        }


    }


static constexpr int maxActiveFilters = 12; // Reduced for simplicity and member size
std::array<juce::dsp::IIR::Coefficients<float>::Ptr, maxActiveFilters> filterCoefficients;
std::array<juce::dsp::IIR::Filter<float>, maxActiveFilters> filters;

float currentSampleRate = 44100.0f;

int sm_rootNote = 0;
int sm_scaleType = 0;
float sm_masterTune = 440.0f;
float sm_harmonicStrength = 0.0f;

// Buffers for UI updates - fixed size to avoid heap alloc
std::array<float, maxActiveFilters> uiFreqs;
std::array<float, maxActiveFilters> uiGains;
std::array<int, 97> uiInKeyMidiNotes; // Max MIDI notes C0-C8 (12-108)
int uiInKeyNotesCount = 0;
int uiActiveFilterCount = 0;
bool needsUiUpdate = false;

// Pre-calculate scale intervals once
std::array<std::array<int, 7>, 7> scales = {
    {{0, 2, 4, 5, 7, 9, 11}}, // Major
    {{0, 2, 3, 5, 7, 9, 10}}, // Minor
    {{0, 2, 3, 5, 7, 9, 10}}, // Dorian
    {{0, 1, 3, 5, 7, 8, 10}}, // Phrygian
    {{0, 2, 4, 6, 7, 9, 11}}, // Lydian
    {{0, 2, 4, 5, 7, 9, 10}}, // Mixolydian
    {{0, 1, 4, 5, 7, 8, 10}}  // Phrygian Dominant
};

// Pre-allocated DynamicObjects for UI messages to avoid heap allocation on audio thread
juce::var eqDataMessage;
juce::var pianoDataMessage;
juce::Array<juce::var> eqFrequenciesArray;
juce::Array<juce::var> eqGainsArray;
juce::Array<juce::var> pianoInKeyNotesArray;

float midiNoteToFrequency(int midiNote, float masterTuneHz)
{
    return masterTuneHz * std::pow(2.0f, (midiNote - 69.0f) / 12.0f);
}

void updateFilterCoefficients()
{
    uiActiveFilterCount = 0;
    uiInKeyNotesCount = 0;

    float strength = sm_harmonicStrength / 100.0f; // 0 to 1

    // Collect in-key and out-of-key MIDI notes
    std::array<int, 97> inKeyMidiNotes;
    std::array<int, 97> outOfKeyMidiNotes;
    int currentInKeyCount = 0;
    int currentOutOfKeyCount = 0;

    for (int midiNote = 12; midiNote <= 108; ++midiNote)
    {
        int noteInOctave = (midiNote - sm_rootNote) % 12;
        if (noteInOctave < 0) noteInOctave += 12;

        bool isInKey = false;
        for (int i = 0; i < 7; ++i)
        {
            if (noteInOctave == scales[sm_scaleType][i])
            {
                isInKey = true;
                break;
            }
        }

        if (isInKey)
        {
            if (currentInKeyCount < inKeyMidiNotes.size())
                inKeyMidiNotes[currentInKeyCount++] = midiNote;
        }
        else
        {
            if (currentOutOfKeyCount < outOfKeyMidiNotes.size())
                outOfKeyMidiNotes[currentOutOfKeyCount++] = midiNote;
        }
    }

    int filterIdx = 0;

    // Apply boosts for in-key notes
    for (int i = 0; i < currentInKeyCount && filterIdx < maxActiveFilters; ++i)
    {
        float freq = midiNoteToFrequency(inKeyMidiNotes[i], sm_masterTune);
        if (freq >= 20.0f && freq <= 20000.0f)
        {
            filterCoefficients[filterIdx] = juce::dsp::IIR::Coefficients<float>::makePeakFilter(currentSampleRate, freq, 2.0f, 1.0f + (1.5f * strength));
            filters[filterIdx].setCoefficients(filterCoefficients[filterIdx]);
            uiFreqs[uiActiveFilterCount] = freq;
            uiGains[uiActiveFilterCount] = juce::Decibels::toDecibels(1.0f + (1.5f * strength));
            uiActiveFilterCount++;
            uiInKeyMidiNotes[uiInKeyNotesCount++] = inKeyMidiNotes[i]; // Store for pianoStrip
            filterIdx++;
        }
    }

    // Apply cuts for out-of-key notes, filling remaining filter slots
    for (int i = 0; i < currentOutOfKeyCount && filterIdx < maxActiveFilters; ++i)
    {
        float freq = midiNoteToFrequency(outOfKeyMidiNotes[i], sm_masterTune);
        if (freq >= 20.0f && freq <= 20000.0f)
        {
            filterCoefficients[filterIdx] = juce::dsp::IIR::Coefficients<float>::makePeakFilter(currentSampleRate, freq, 1.5f, 1.0f - (0.8f * strength));
            filters[filterIdx].setCoefficients(filterCoefficients[filterIdx]);
            uiFreqs[uiActiveFilterCount] = freq;
            uiGains[uiActiveFilterCount] = juce::Decibels::toDecibels(1.0f - (0.8f * strength));
            uiActiveFilterCount++;
            filterIdx++;
        }
    }

    // Bypass unused filters
    for (; filterIdx < maxActiveFilters; ++filterIdx)
    {
        filterCoefficients[filterIdx] = juce::dsp::IIR::Coefficients<float>::makePeakFilter(currentSampleRate, 1000.0f, 0.707f, 1.0f);
        filters[filterIdx].setCoefficients(filterCoefficients[filterIdx]);
    }

    needsUiUpdate = true;
}
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;
    std::atomic<bool> keyScaleCoeffsDirty { true };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Harmonic2KProcessor)
};
