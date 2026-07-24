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


class HarmonicEQProcessor : public juce::AudioProcessor
{
public:
    HarmonicEQProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "PARAMS", createLayout())
    {
        _mm_setcsr (_mm_getcsr() | 0x8040);

        licensed = HarmonicEQLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("HarmonicEQ: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("root", 0.0f, 1);
        versionManager.registerParameter ("scale", 0.0f, 1);
        versionManager.registerParameter ("strength", 100.0f, 1);
        versionManager.registerParameter ("tune", 440.0f, 1);
        versionManager.registerParameter ("gain", 0.0f, 1);
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "root", 1 }, "Root Note", juce::NormalisableRange<float> (0.0f, 11.0f), 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "scale", 1 }, "Scale Type", juce::NormalisableRange<float> (0.0f, 6.0f), 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "strength", 1 }, "Harmonic Strength", juce::NormalisableRange<float> (0.0f, 100.0f), 100.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "tune", 1 }, "Master Tune", juce::NormalisableRange<float> (432.0f, 444.0f), 440.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = ${SEED}u; // seed lock: reproducible renders




        sm_root.init (apvts.getRawParameterValue ("root")->load(), 20.0f, (float) sampleRate);
        sm_scale.init (apvts.getRawParameterValue ("scale")->load(), 20.0f, (float) sampleRate);
        sm_strength.init (apvts.getRawParameterValue ("strength")->load(), 20.0f, (float) sampleRate);
        sm_tune.init (apvts.getRawParameterValue ("tune")->load(), 20.0f, (float) sampleRate);
        sm_gain.init (apvts.getRawParameterValue ("gain")->load(), 20.0f, (float) sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
            currentSampleRate = (float) getSampleRate();
    for (int i = 0; i < maxNumFilters; ++i)
    {
        coefficients[i] = juce::dsp::IIR::Coefficients<float>::makePeakFilter(currentSampleRate, 1000.0f, 1.0f, 1.0f);
        filters[i].setCoefficients(coefficients[i]);
        filters[i].reset();
    }
    smoothingCoeff = 1.0f - std::exp(-1.0f / (0.01f * currentSampleRate)); // 10ms smoothing time
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
        // ---- custom block: Harmonic EQ (AI-generated) ----
    const float strengthParam = apvts.getRawParameterValue("strength")->load();
    const float tuneParam = apvts.getRawParameterValue("tune")->load();
    const int rootParam = static_cast<int>(apvts.getRawParameterValue("root")->load());
    const int scaleParam = static_cast<int>(apvts.getRawParameterValue("scale")->load());

    targetStrength = strengthParam / 100.0f; // 0.0 to 1.0
    targetTune = tuneParam;

    smoothStrength += smoothingCoeff * (targetStrength - smoothStrength);
    smoothTune += smoothingCoeff * (targetTune - smoothTune);

    // Recalculate filters only if parameters change significantly
    static float lastStrength = -1.0f, lastTune = -1.0f, lastRoot = -1.0f, lastScale = -1.0f;
    if (std::abs(smoothStrength - lastStrength) > 0.001f ||
        std::abs(smoothTune - lastTune) > 0.01f ||
        rootParam != static_cast<int>(lastRoot) ||
        scaleParam != static_cast<int>(lastScale))
    {
        lastStrength = smoothStrength;
        lastTune = smoothTune;
        lastRoot = static_cast<float>(rootParam);
        lastScale = static_cast<float>(scaleParam);

        numActiveFilters = 0;
        const float a4Freq = smoothTune; // A4 frequency
        const int a4Midi = 69;

        // Determine in-key MIDI notes
        std::array<bool, 12> isInKey = {false};
        const auto& intervals = scaleIntervals[scaleParam];
        for (int interval : intervals)
        {
            isInKey[(rootParam + interval) % 12] = true;
        }

        // Iterate through MIDI notes from 20Hz to 20kHz
        for (int midiNote = 0; midiNote <= 127; ++midiNote)
        {
            float freq = a4Freq * std::pow(2.0f, (midiNote - a4Midi) / 12.0f);

            if (freq >= 20.0f && freq <= 20000.0f)
            {
                if (numActiveFilters < maxNumFilters)
                {
                    float gainDb;
                    float q;

                    if (isInKey[midiNote % 12])
                    {
                        gainDb = 1.5f * smoothStrength; // Boost
                        q = 2.0f;
                    }
                    else
                    {
                        gainDb = -0.8f * smoothStrength; // Cut
                        q = 1.5f;
                    }

                    coefficients[numActiveFilters] = juce::dsp::IIR::Coefficients<float>::makePeakFilter(currentSampleRate, freq, q, juce::Decibels::decibelsToGain(gainDb));
                    filters[numActiveFilters].setCoefficients(coefficients[numActiveFilters]);
                    activeFrequencies[numActiveFilters] = freq;
                    numActiveFilters++;
                }
            }
        }
    }

    for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
    {
        auto* channelData = block.getChannelPointer((int) ch);
        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            float sample = channelData[i];
            for (int k = 0; k < numActiveFilters; ++k)
            {
                sample = filters[k].processSample(sample);
            }
            channelData[i] = sample;
        }
    }
        gainDsp.setGainDecibels (smoothedParam ("gain"));
    gainDsp.process (ctx);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ScopedNoDenormals noDenormals;
        sm_root.setTarget (apvts.getRawParameterValue ("root")->load());
        sm_scale.setTarget (apvts.getRawParameterValue ("scale")->load());
        sm_strength.setTarget (apvts.getRawParameterValue ("strength")->load());
        sm_tune.setTarget (apvts.getRawParameterValue ("tune")->load());
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
    const juce::String getName() const override { return "HarmonicEQ"; }
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
    ParameterSmoother sm_root;
    ParameterSmoother sm_scale;
    ParameterSmoother sm_strength;
    ParameterSmoother sm_tune;
    ParameterSmoother sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one control-block step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "root") == 0) return sm_root.nextValue();
        if (strcmp (id, "scale") == 0) return sm_scale.nextValue();
        if (strcmp (id, "strength") == 0) return sm_strength.nextValue();
        if (strcmp (id, "tune") == 0) return sm_tune.nextValue();
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
        if (auto* param = apvts.getParameter ("root")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (100.0f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (440.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("root")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (439.9752f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-0.0239f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("root")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (439.7212f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (3.4771f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("root")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (440.1216f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-0.7881f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("root")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (439.136f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-3.3221f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("root")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (440.36f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (2.9451f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("root")) param->setValueNotifyingHost (param->convertTo0to1 (3.2849f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (1.3953f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (18.698f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (443.1447f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (24.0f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("root")) param->setValueNotifyingHost (param->convertTo0to1 (6.899f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (3.7681f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (23.4861f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (437.1318f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-3.4682f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("root")) param->setValueNotifyingHost (param->convertTo0to1 (4.9995f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (1.3343f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (18.6004f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (433.391f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-15.5743f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("root")) param->setValueNotifyingHost (param->convertTo0to1 (5.9713f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (3.5898f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (33.8566f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (436.0633f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (5.7222f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("root")) param->setValueNotifyingHost (param->convertTo0to1 (6.1411f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (4.189f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (94.4775f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (440.6161f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-14.54f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("root")) param->setValueNotifyingHost (param->convertTo0to1 (10.6558f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (0.2681f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (0.5789f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (432.5087f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-20.524f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("root")) param->setValueNotifyingHost (param->convertTo0to1 (0.614f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (5.6454f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (1.9296f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (432.6809f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (23.1962f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("root")) param->setValueNotifyingHost (param->convertTo0to1 (10.7732f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (0.0706f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (5.0296f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (432.3261f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (21.1059f));
        break;
        default: break;
        }


    }


    static constexpr int maxNumFilters = 24;
    std::array<juce::dsp::IIR::Filter<float>, maxNumFilters> filters;
    std::array<juce::dsp::IIR::Coefficients<float>::Ptr, maxNumFilters> coefficients;

    float currentSampleRate = 0.0f;

    // Parameters for smoothing
    float smoothStrength = 0.0f;
    float targetStrength = 0.0f;
    float smoothTune = 440.0f;
    float targetTune = 440.0f;

    // Note frequencies based on A4=440Hz, equal temperament
    // MIDI note 0 is C-1 (8.175799 Hz), MIDI note 127 is G9 (12543.854 Hz)
    // We'll calculate frequencies dynamically based on 'tune' param

    // Scale definitions (MIDI note offsets from root)
    // Major: 0, 2, 4, 5, 7, 9, 11
    // Natural Minor: 0, 2, 3, 5, 7, 8, 10
    // Dorian: 0, 2, 3, 5, 7, 9, 10
    // Phrygian: 0, 1, 3, 5, 7, 8, 10
    // Lydian: 0, 2, 4, 6, 7, 9, 11
    // Mixolydian: 0, 2, 4, 5, 7, 9, 10
    // Phrygian Dominant: 0, 1, 4, 5, 7, 8, 10
    std::array<std::array<int, 7>, 7> scaleIntervals = {
        {0, 2, 4, 5, 7, 9, 11},   // Major
        {0, 2, 3, 5, 7, 8, 10},   // Natural Minor
        {0, 2, 3, 5, 7, 9, 10},   // Dorian
        {0, 1, 3, 5, 7, 8, 10},   // Phrygian
        {0, 2, 4, 6, 7, 9, 11},   // Lydian
        {0, 2, 4, 5, 7, 9, 10},   // Mixolydian
        {0, 1, 4, 5, 7, 8, 10}    // Phrygian Dominant
    };

    // Store calculated frequencies for active filters
    std::array<float, maxNumFilters> activeFrequencies;
    int numActiveFilters = 0;

    // Smoothing coefficients for parameters
    float smoothingCoeff = 0.0f;
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HarmonicEQProcessor)
};
