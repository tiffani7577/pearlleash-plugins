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


class SoftTouchProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    SoftTouchProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, juce::Identifier ("PARAMS"), createLayout())
    {
        // Portable FTZ (x86 MXCSR / arm64 FPCR). processBlock also uses juce::ScopedNoDenormals.
        pfEnableFlushToZero();

        licensed = SoftTouchLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("SoftTouch: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("drive", 0.15f, 1);
        versionManager.registerParameter ("warmth", 0.5f, 1);
        versionManager.registerParameter ("presence", 0.5f, 1);
        versionManager.registerParameter ("character", 0.5f, 1);
        versionManager.registerParameter ("mix", 1.0f, 1);
        versionManager.registerParameter ("trim", 0.0f, 1);
        versionManager.registerParameter ("gain", 0.0f, 1);


    }

    ~SoftTouchProcessor() override
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
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "drive", 1 }, "Drive", juce::NormalisableRange<float> (0.0f, 1.0f), 0.15f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "warmth", 1 }, "Warmth", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "presence", 1 }, "Presence", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "character", 1 }, "Character", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "trim", 1 }, "Output Trim", juce::NormalisableRange<float> (-12.0f, 12.0f), 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = 1347570764u; // seed lock: reproducible renders




        sm_drive.reset (apvts.getRawParameterValue ("drive")->load(), 10.0f, sampleRate);
        sm_warmth.reset (apvts.getRawParameterValue ("warmth")->load(), 10.0f, sampleRate);
        sm_presence.reset (apvts.getRawParameterValue ("presence")->load(), 10.0f, sampleRate);
        sm_character.reset (apvts.getRawParameterValue ("character")->load(), 10.0f, sampleRate);
        sm_mix.reset (apvts.getRawParameterValue ("mix")->load(), 5.0f, sampleRate);
        sm_trim.reset (apvts.getRawParameterValue ("trim")->load(), 10.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        srF = (float) sampleRate;
driveSmoothed.reset(srF, 0.020);
driveSmoothed.setCurrentAndTargetValue(0.15f);
warmthSmoothed.reset(srF, 0.020);
warmthSmoothed.setCurrentAndTargetValue(0.5f);
presenceSmoothed.reset(srF, 0.020);
presenceSmoothed.setCurrentAndTargetValue(0.5f);
characterSmoothed.reset(srF, 0.020);
characterSmoothed.setCurrentAndTargetValue(0.5f);
mixSmoothed.reset(srF, 0.020);
mixSmoothed.setCurrentAndTargetValue(1.0f);
trimSmoothed.reset(srF, 0.020);
trimSmoothed.setCurrentAndTargetValue(0.0f);
for (size_t c = 0; c < (size_t) 2; ++c) { warmthState[c] = 0.0f; presenceState[c] = 0.0f; for (size_t k = 0; k < (size_t) 8; ++k) { upHistory[c][k] = 0.0f; downHistory[c][k] = 0.0f; } }
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
        // ---- custom block: SoftTouch (AI-generated) ----
const float rawDrive = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("drive")->load());
const float rawWarmth = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("warmth")->load());
const float rawPresence = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("presence")->load());
const float rawCharacter = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("character")->load());
const float rawMix = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("mix")->load());
const float rawTrim = juce::jlimit(-12.0f, 12.0f, apvts.getRawParameterValue("trim")->load());
driveSmoothed.setTargetValue(rawDrive);
warmthSmoothed.setTargetValue(rawWarmth);
presenceSmoothed.setTargetValue(rawPresence);
characterSmoothed.setTargetValue(rawCharacter);
mixSmoothed.setTargetValue(rawMix);
trimSmoothed.setTargetValue(rawTrim);
const float wf0 = 250.0f;
const float pf0 = 3000.0f;
const float wG = std::tan(juce::MathConstants<float>::pi * wf0 / srF);
const float pG = std::tan(juce::MathConstants<float>::pi * pf0 / srF);
for (size_t ch = 0; ch < block.getNumChannels(); ++ch) {
  auto* d = block.getChannelPointer((int)ch);
  int ci = (int)ch < 2 ? (int)ch : 1;
  for (size_t i = 0; i < block.getNumSamples(); ++i) {
    const float drv = driveSmoothed.getNextValue();
    const float wrm = warmthSmoothed.getNextValue();
    const float prs = presenceSmoothed.getNextValue();
    const float chr = characterSmoothed.getNextValue();
    const float dScaled = 0.01f + drv * drv * 7.99f;
    const float tanhD = std::tanh(dScaled);
    const float gComp = (tanhD > 1e-6f) ? (1.0f / tanhD) : 1.0f;
    float inputSample = d[i];
    float x = inputSample;
    float acc = 0.0f;
    for (int k = 7; k > 0; --k) upHistory[ci][k] = upHistory[ci][k-1];
    upHistory[ci][0] = x;
    for (size_t os = 0; os < (size_t) OS; ++os) {
      float up = (os == 0) ? x * (float)OS : 0.0f;
      float sat = std::tanh(dScaled * up);
      if (tanhD > 1e-6f) sat /= tanhD;
      float y2 = sat * sat;
      float yChar = sat + chr * 0.15f * y2;
      float peak = 1.0f + chr * 0.15f;
      if (peak > 1e-6f) yChar /= peak;
      acc += yChar;
    }
    float processed = acc * (1.0f / (float)OS);
    processed *= gComp;
    const float wAmp = std::pow(10.0f, (wrm * 6.0f) / 20.0f);
    const float wA = wG * wAmp;
    const float wDenom = 1.0f + wG;
    const float wv = (processed - warmthState[ci]) / wDenom;
    const float wLow = wv * wG + warmthState[ci];
    warmthState[ci] += 2.0f * wv * wG;
    float wBlend = wLow * wAmp + (processed - wLow);
    processed = wBlend;
    const float pAmp = std::pow(10.0f, (prs * 6.0f) / 20.0f);
    const float pDenom = 1.0f + pG;
    const float pv = (processed - presenceState[ci]) / pDenom;
    const float pLow = pv * pG + presenceState[ci];
    presenceState[ci] += 2.0f * pv * pG;
    float pHigh = processed - pLow;
    float pBlend = pLow + pHigh * pAmp;
    processed = pBlend;
    const float trimGain = std::pow(10.0f, trimSmoothed.getNextValue() / 20.0f);
    processed *= trimGain;
    float dry = inputSample;
    float wet = processed;
    float output = dry * (1.0f - mixSmoothed.getNextValue()) + wet * mixSmoothed.getNextValue();
    d[i] = output;
  }
}
        gainDsp.setGainDecibels (smoothedParam ("gain"));
    gainDsp.process (ctx);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ignoreUnused (midiMessages);
        juce::ScopedNoDenormals noDenormals;
        sm_drive.setTarget (apvts.getRawParameterValue ("drive")->load());
        sm_drive.update (getSampleRate());
        sm_warmth.setTarget (apvts.getRawParameterValue ("warmth")->load());
        sm_warmth.update (getSampleRate());
        sm_presence.setTarget (apvts.getRawParameterValue ("presence")->load());
        sm_presence.update (getSampleRate());
        sm_character.setTarget (apvts.getRawParameterValue ("character")->load());
        sm_character.update (getSampleRate());
        sm_mix.setTarget (apvts.getRawParameterValue ("mix")->load());
        sm_mix.update (getSampleRate());
        sm_trim.setTarget (apvts.getRawParameterValue ("trim")->load());
        sm_trim.update (getSampleRate());
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
    const juce::String getName() const override { return "SoftTouch"; }
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
    WoManusSmoothedParameter<float> sm_drive;
    WoManusSmoothedParameter<float> sm_warmth;
    WoManusSmoothedParameter<float> sm_presence;
    WoManusSmoothedParameter<float> sm_character;
    WoManusSmoothedParameter<float> sm_mix;
    WoManusSmoothedParameter<float> sm_trim;
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "drive") == 0) return sm_drive.getNextValue();
        if (strcmp (id, "warmth") == 0) return sm_warmth.getNextValue();
        if (strcmp (id, "presence") == 0) return sm_presence.getNextValue();
        if (strcmp (id, "character") == 0) return sm_character.getNextValue();
        if (strcmp (id, "mix") == 0) return sm_mix.getNextValue();
        if (strcmp (id, "trim") == 0) return sm_trim.getNextValue();
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
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.15f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("character")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("trim")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.4041f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.554f));
        if (auto* param = apvts.getParameter ("character")) param->setValueNotifyingHost (param->convertTo0to1 (0.4615f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("trim")) param->setValueNotifyingHost (param->convertTo0to1 (0.1608f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-2.7323f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.5552f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.5782f));
        if (auto* param = apvts.getParameter ("character")) param->setValueNotifyingHost (param->convertTo0to1 (0.4858f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("trim")) param->setValueNotifyingHost (param->convertTo0to1 (-0.4964f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-1.1524f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.4285f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.5307f));
        if (auto* param = apvts.getParameter ("character")) param->setValueNotifyingHost (param->convertTo0to1 (0.543f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("trim")) param->setValueNotifyingHost (param->convertTo0to1 (-0.9872f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.9275f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.5159f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.5049f));
        if (auto* param = apvts.getParameter ("character")) param->setValueNotifyingHost (param->convertTo0to1 (0.5139f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("trim")) param->setValueNotifyingHost (param->convertTo0to1 (1.2837f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.8275f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.5903f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.5772f));
        if (auto* param = apvts.getParameter ("character")) param->setValueNotifyingHost (param->convertTo0to1 (0.4126f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("trim")) param->setValueNotifyingHost (param->convertTo0to1 (1.4646f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (4.0013f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7126f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.9161f));
        if (auto* param = apvts.getParameter ("character")) param->setValueNotifyingHost (param->convertTo0to1 (0.4745f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4065f));
        if (auto* param = apvts.getParameter ("trim")) param->setValueNotifyingHost (param->convertTo0to1 (2.7855f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (5.0873f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.192f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.2528f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.6692f));
        if (auto* param = apvts.getParameter ("character")) param->setValueNotifyingHost (param->convertTo0to1 (0.5467f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.3917f));
        if (auto* param = apvts.getParameter ("trim")) param->setValueNotifyingHost (param->convertTo0to1 (4.3648f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-13.1594f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3893f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.8152f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.7274f));
        if (auto* param = apvts.getParameter ("character")) param->setValueNotifyingHost (param->convertTo0to1 (0.27f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4683f));
        if (auto* param = apvts.getParameter ("trim")) param->setValueNotifyingHost (param->convertTo0to1 (-9.9735f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (13.9439f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.7713f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7446f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.3837f));
        if (auto* param = apvts.getParameter ("character")) param->setValueNotifyingHost (param->convertTo0to1 (0.4267f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7142f));
        if (auto* param = apvts.getParameter ("trim")) param->setValueNotifyingHost (param->convertTo0to1 (4.5567f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (13.1193f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.5184f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7752f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.9927f));
        if (auto* param = apvts.getParameter ("character")) param->setValueNotifyingHost (param->convertTo0to1 (0.7587f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.3377f));
        if (auto* param = apvts.getParameter ("trim")) param->setValueNotifyingHost (param->convertTo0to1 (0.8069f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (13.4857f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.0727f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.0745f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.0525f));
        if (auto* param = apvts.getParameter ("character")) param->setValueNotifyingHost (param->convertTo0to1 (0.9276f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9686f));
        if (auto* param = apvts.getParameter ("trim")) param->setValueNotifyingHost (param->convertTo0to1 (-10.2163f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-23.9026f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.0343f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.9916f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.9973f));
        if (auto* param = apvts.getParameter ("character")) param->setValueNotifyingHost (param->convertTo0to1 (0.0683f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.033f));
        if (auto* param = apvts.getParameter ("trim")) param->setValueNotifyingHost (param->convertTo0to1 (11.4286f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (20.6067f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.0292f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.0694f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.0688f));
        if (auto* param = apvts.getParameter ("character")) param->setValueNotifyingHost (param->convertTo0to1 (0.0105f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.953f));
        if (auto* param = apvts.getParameter ("trim")) param->setValueNotifyingHost (param->convertTo0to1 (11.4125f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-23.5561f));
        break;
        default: break;
        }


    }


juce::SmoothedValue<float> driveSmoothed;
juce::SmoothedValue<float> warmthSmoothed;
juce::SmoothedValue<float> presenceSmoothed;
juce::SmoothedValue<float> characterSmoothed;
juce::SmoothedValue<float> mixSmoothed;
juce::SmoothedValue<float> trimSmoothed;
float srF = 44100.0f;
float warmthState[2] = {0.0f, 0.0f};
float presenceState[2] = {0.0f, 0.0f};
static constexpr int OS = 4;
float osBuffer[OS] = {};
static constexpr float FIR_UP[4] = {0.25f, 0.25f, 0.25f, 0.25f};
float upHistory[2][8] = {};
float downHistory[2][8] = {};
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoftTouchProcessor)
};
