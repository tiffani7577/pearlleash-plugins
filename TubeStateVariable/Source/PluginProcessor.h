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


class TubeStateVariableProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    TubeStateVariableProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, juce::Identifier ("PARAMS"), createLayout())
    {
        // Portable FTZ (x86 MXCSR / arm64 FPCR). processBlock also uses juce::ScopedNoDenormals.
        pfEnableFlushToZero();

        licensed = TubeStateVariableLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("TubeStateVariable: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("cutoff", 800.0f, 1);
        versionManager.registerParameter ("resonance", 0.5f, 1);
        versionManager.registerParameter ("warmth", 0.3f, 1);
        versionManager.registerParameter ("drive", 1.5f, 1);
        versionManager.registerParameter ("mix", 1.0f, 1);
        versionManager.registerParameter ("gain", 1.0f, 1);


    }

    ~TubeStateVariableProcessor() override
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
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "cutoff", 1 }, "Cutoff", juce::NormalisableRange<float> (20.0f, 20000.0f), 800.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "resonance", 1 }, "Resonance", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "warmth", 1 }, "Tube Warmth", juce::NormalisableRange<float> (0.0f, 1.0f), 0.3f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "drive", 1 }, "Drive", juce::NormalisableRange<float> (0.5f, 8.0f), 1.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output Gain", juce::NormalisableRange<float> (0.0f, 2.0f), 1.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = 1347570764u; // seed lock: reproducible renders




        sm_cutoff.reset (apvts.getRawParameterValue ("cutoff")->load(), 30.0f, sampleRate);
        sm_resonance.reset (apvts.getRawParameterValue ("resonance")->load(), 10.0f, sampleRate);
        sm_warmth.reset (apvts.getRawParameterValue ("warmth")->load(), 10.0f, sampleRate);
        sm_drive.reset (apvts.getRawParameterValue ("drive")->load(), 10.0f, sampleRate);
        sm_mix.reset (apvts.getRawParameterValue ("mix")->load(), 5.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        srF = (float) sampleRate;
cutoffSmooth.reset(srF, 0.020);
cutoffSmooth.setCurrentAndTargetValue(800.0f);
resonanceSmooth.reset(srF, 0.020);
resonanceSmooth.setCurrentAndTargetValue(0.5f);
warmthSmooth.reset(srF, 0.020);
warmthSmooth.setCurrentAndTargetValue(0.3f);
driveSmooth.reset(srF, 0.020);
driveSmooth.setCurrentAndTargetValue(1.5f);
mixSmoothed.reset(srF, 0.020);
mixSmoothed.setCurrentAndTargetValue(1.0f);
gainSmooth.reset(srF, 0.020);
gainSmooth.setCurrentAndTargetValue(1.0f);
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
        // ---- custom block: TubeStateVariable (AI-generated) ----
const float bpm = juce::jmax(1.0f, getBpm());
(void)bpm;
const float rawCutoff = juce::jlimit(20.0f, 20000.0f, apvts.getRawParameterValue("cutoff")->load());
const float rawRes = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("resonance")->load());
const float rawWarmth = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("warmth")->load());
const float rawDrive = juce::jlimit(0.5f, 8.0f, apvts.getRawParameterValue("drive")->load());
const float rawMix = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("mix")->load());
const float rawGain = juce::jlimit(0.0f, 2.0f, apvts.getRawParameterValue("gain")->load());
cutoffSmooth.setTargetValue(rawCutoff);
resonanceSmooth.setTargetValue(rawRes);
warmthSmooth.setTargetValue(rawWarmth);
driveSmooth.setTargetValue(rawDrive);
mixSmoothed.setTargetValue(rawMix);
gainSmooth.setTargetValue(rawGain);
const float srOs = srF * 4.0f;
const int nSamples = (int) block.getNumSamples();
const int nCh = (int) block.getNumChannels();
for (size_t i = 0; i < (size_t) nSamples; ++i) {
  const float fc = cutoffSmooth.getNextValue();
  const float res = resonanceSmooth.getNextValue();
  const float wrm = warmthSmooth.getNextValue();
  const float drv = driveSmooth.getNextValue();
  const float gn = gainSmooth.getNextValue();
  const float m = mixSmoothed.getNextValue();
  const float g = tanf(juce::MathConstants<float>::pi * juce::jlimit(20.0f, srOs * 0.49f, fc) / srOs);
  const float k = juce::jlimit(0.001f, 2.0f, 2.0f - 2.0f * res);
  for (size_t ch = 0; ch < (size_t) nCh && ch < 2; ++ch) {
    auto* d = block.getChannelPointer(ch);
    float dry = d[i];
    float xIn = dry;
    float wet = dry;
    for (size_t os = 0; os < (size_t) 4; ++os) {
      float x = xIn;
      float hp = (x - k * ic1eq[ch] - ic2eq[ch]) / (1.0f + g * k + g * g);
      float bp = g * hp + ic1eq[ch];
      float lp = g * bp + ic2eq[ch];
      ic1eq[ch] = 2.0f * bp - ic1eq[ch];
      ic2eq[ch] = 2.0f * lp - ic2eq[ch];
      float mixed = 0.5f * lp + 0.3f * bp + 0.2f * hp;
      float sat = tubeSat(mixed, drv, wrm * 2.0f);
      xIn = sat;
      if (os == 3) wet = sat * gn;
    }
    float dryS = dry;
    float wetS = wet;
    float output = dryS * (1.0f - m) + wetS * m;
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
        sm_cutoff.setTarget (apvts.getRawParameterValue ("cutoff")->load());
        sm_cutoff.update (getSampleRate());
        sm_resonance.setTarget (apvts.getRawParameterValue ("resonance")->load());
        sm_resonance.update (getSampleRate());
        sm_warmth.setTarget (apvts.getRawParameterValue ("warmth")->load());
        sm_warmth.update (getSampleRate());
        sm_drive.setTarget (apvts.getRawParameterValue ("drive")->load());
        sm_drive.update (getSampleRate());
        sm_mix.setTarget (apvts.getRawParameterValue ("mix")->load());
        sm_mix.update (getSampleRate());
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
    const juce::String getName() const override { return "TubeStateVariable"; }
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
    WoManusSmoothedParameter<float> sm_cutoff;
    WoManusSmoothedParameter<float> sm_resonance;
    WoManusSmoothedParameter<float> sm_warmth;
    WoManusSmoothedParameter<float> sm_drive;
    WoManusSmoothedParameter<float> sm_mix;
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "cutoff") == 0) return sm_cutoff.getNextValue();
        if (strcmp (id, "resonance") == 0) return sm_resonance.getNextValue();
        if (strcmp (id, "warmth") == 0) return sm_warmth.getNextValue();
        if (strcmp (id, "drive") == 0) return sm_drive.getNextValue();
        if (strcmp (id, "mix") == 0) return sm_mix.getNextValue();
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
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (800.0f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (6014.0f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.5766f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.3071f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (2.75f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.0639f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (6014.0f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.5745f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.3469f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (2.75f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.0085f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (6014.0f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.5067f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (2.75f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.1591f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (6014.0f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.4761f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (2.75f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.1681f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (6014.0f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.4242f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (2.75f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.8524f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (6135.2299f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.6066f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7769f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (2.5801f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9049f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.0087f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (5837.6066f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.3086f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.1914f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (2.1118f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.2237f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.2414f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (12296.3291f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.3146f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.6525f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (3.7536f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.3675f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.1823f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (13160.9148f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.4563f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.5927f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (3.1235f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.6048f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.6715f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (18935.1901f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.0588f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.4076f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (7.4941f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.6251f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.8292f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (19125.6047f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.0756f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.0151f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.5674f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0319f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0756f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (19973.7118f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.0783f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.9405f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (7.735f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0249f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.1387f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (1493.1404f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.0013f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.936f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (7.7151f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0211f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.8998f));
        break;
        default: break;
        }


    }


juce::SmoothedValue<float> cutoffSmooth;
juce::SmoothedValue<float> resonanceSmooth;
juce::SmoothedValue<float> warmthSmooth;
juce::SmoothedValue<float> driveSmooth;
juce::SmoothedValue<float> mixSmoothed;
juce::SmoothedValue<float> gainSmooth;
float ic1eq[2] = {0.0f, 0.0f};
float ic2eq[2] = {0.0f, 0.0f};
float srF = 44100.0f;
inline float tubeSat(float x, float drv, float wrm) {
  float d = juce::jlimit(0.01f, 20.0f, drv);
  float w = juce::jlimit(0.0f, 4.0f, wrm);
  float norm = tanhf(d);
  if (norm < 1e-6f) norm = 1e-6f;
  return tanhf(d * x + w * x * x) / norm;
}
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TubeStateVariableProcessor)
};
