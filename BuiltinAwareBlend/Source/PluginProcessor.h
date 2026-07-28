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


class BuiltinAwareBlendProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    BuiltinAwareBlendProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, juce::Identifier ("PARAMS"), createLayout())
    {
        // Portable FTZ (x86 MXCSR / arm64 FPCR). processBlock also uses juce::ScopedNoDenormals.
        pfEnableFlushToZero();

        licensed = BuiltinAwareBlendLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("BuiltinAwareBlend: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("mixAmount", 1.0f, 1);
        versionManager.registerParameter ("gain", 0.0f, 1);


    }

    ~BuiltinAwareBlendProcessor() override
    {
        // no parameter listeners registered
    }

    void parameterChanged (const juce::String& parameterID, float newValue) override
    {
        juce::ignoreUnused (parameterID, newValue);

    }


    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mixAmount", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = 1347570764u; // seed lock: reproducible renders




        sm_mixAmount.reset (apvts.getRawParameterValue ("mixAmount")->load(), 5.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        srF = (float) sampleRate;
mixSmoothed.reset(srF, 0.020f);
mixSmoothed.setCurrentAndTargetValue(1.0f);
gainSmoothed.reset(srF, 0.020f);
gainSmoothed.setCurrentAndTargetValue(1.0f);
cutoffSmoothed.reset(srF, 0.020f);
cutoffSmoothed.setCurrentAndTargetValue(800.0f);
resonanceSmoothed.reset(srF, 0.020f);
resonanceSmoothed.setCurrentAndTargetValue(0.5f);
driveSmoothed.reset(srF, 0.020f);
driveSmoothed.setCurrentAndTargetValue(1.5f);
for (size_t c = 0; c < (size_t) 2; ++c) { ic1eq[c] = 0.0f; ic2eq[c] = 0.0f; }
            gainDsp.prepare (dspSpec); gainDsp.setRampDurationSeconds (0.02);
        truePeakLeft.prepare ((float) sampleRate);
        truePeakRight.prepare ((float) sampleRate);
        analyzer.prepare (sampleRate);
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

        truePeakLeft.reset();
        truePeakRight.reset();
    }

    void processChain (juce::dsp::AudioBlock<float> block)
    {
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        juce::ignoreUnused (ctx);
        // ---- custom block: BuiltinAwareBlend (AI-generated) ----
const float rawMix = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("mixAmount")->load());
mixSmoothed.setTargetValue(rawMix);
gainSmoothed.setTargetValue(1.0f);
cutoffSmoothed.setTargetValue(800.0f);
resonanceSmoothed.setTargetValue(0.5f);
driveSmoothed.setTargetValue(1.5f);
const size_t numCh = block.getNumChannels();
const size_t numSamples = block.getNumSamples();
for (size_t i = 0; i < numSamples; ++i) {
  const float mixVal = mixSmoothed.getNextValue();
  const float gainVal = gainSmoothed.getNextValue();
  const float cutoff = juce::jlimit(20.0f, 20000.0f, cutoffSmoothed.getNextValue());
  const float res = juce::jlimit(0.0f, 0.99f, resonanceSmoothed.getNextValue());
  const float drv = driveSmoothed.getNextValue();
  const float g = tanf(juce::MathConstants<float>::pi * cutoff / srF);
  const float k = 2.0f - 2.0f * res;
  for (size_t ch = 0; ch < numCh; ++ch) {
    auto* d = block.getChannelPointer((int)ch);
    const int c = (int)ch < 2 ? (int)ch : 1;
    float inputSample = d[i];
    float x_sat = tubeSat(inputSample, drv);
    float v1 = (g * (x_sat - ic2eq[c]) + ic1eq[c]) / (1.0f + g * (g + k));
    float v2 = ic2eq[c] + g * v1;
    ic1eq[c] += 2.0f * g * v1 - k * ic2eq[c];
    ic2eq[c] += 2.0f * v2;
    float processedSample = v2 * gainVal;
    float dry = inputSample;
    float wet = processedSample;
    d[i] = dry * (1.0f - mixVal) + wet * mixVal;
  }
}
        gainDsp.setGainDecibels (smoothedParam ("gain"));
    gainDsp.process (ctx);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ignoreUnused (midiMessages);
        juce::ScopedNoDenormals noDenormals;
        sm_mixAmount.setTarget (apvts.getRawParameterValue ("mixAmount")->load());
        sm_mixAmount.update (getSampleRate());
        sm_gain.setTarget (apvts.getRawParameterValue ("gain")->load());
        sm_gain.update (getSampleRate());
        analyzer.pushBuffer (buffer);



        processChain (juce::dsp::AudioBlock<float> (buffer));
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
                    float x = d[i] * trim;
                    const float ax = std::abs (x);
                    if (ax > knee)
                        x = (x > 0.0f ? 1.0f : -1.0f)
                            * (knee + (ceil_ - knee) * std::tanh ((ax - knee) / (ceil_ - knee)));
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
    const juce::String getName() const override { return "BuiltinAwareBlend"; }
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
    WoManusSmoothedParameter<float> sm_mixAmount;
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "mixAmount") == 0) return sm_mixAmount.getNextValue();
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
        if (auto* param = apvts.getParameter ("mixAmount")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("mixAmount")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-3.7377f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("mixAmount")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-1.9505f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("mixAmount")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-2.0932f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("mixAmount")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-2.5349f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("mixAmount")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-1.4191f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("mixAmount")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (4.1426f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("mixAmount")) param->setValueNotifyingHost (param->convertTo0to1 (0.2204f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-7.0295f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("mixAmount")) param->setValueNotifyingHost (param->convertTo0to1 (0.4892f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-9.8335f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("mixAmount")) param->setValueNotifyingHost (param->convertTo0to1 (0.4582f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (7.262f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("mixAmount")) param->setValueNotifyingHost (param->convertTo0to1 (0.7639f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-14.0687f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("mixAmount")) param->setValueNotifyingHost (param->convertTo0to1 (0.9517f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-20.2002f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("mixAmount")) param->setValueNotifyingHost (param->convertTo0to1 (0.0276f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (23.0179f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("mixAmount")) param->setValueNotifyingHost (param->convertTo0to1 (0.0286f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-22.141f));
        break;
        default: break;
        }


    }


juce::SmoothedValue<float> mixSmoothed;
juce::SmoothedValue<float> gainSmoothed;
juce::SmoothedValue<float> cutoffSmoothed;
juce::SmoothedValue<float> resonanceSmoothed;
juce::SmoothedValue<float> driveSmoothed;
float ic1eq[2] = {0.0f, 0.0f};
float ic2eq[2] = {0.0f, 0.0f};
float srF = 44100.0f;
inline float tubeSat(float x, float drv) {
  float d = juce::jlimit(0.01f, 20.0f, drv);
  float n = tanhf(d);
  if (n < 1e-6f) n = 1e-6f;
  return tanhf(d * x) / n;
}
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BuiltinAwareBlendProcessor)
};
