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


class StutterGateProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    StutterGateProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, juce::Identifier ("PARAMS"), createLayout())
    {
        // Portable FTZ (x86 MXCSR / arm64 FPCR). processBlock also uses juce::ScopedNoDenormals.
        pfEnableFlushToZero();

        licensed = StutterGateLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("StutterGate: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("activePattern", 0.0f, 1);
        versionManager.registerParameter ("subdivision", 3.0f, 1);
        versionManager.registerParameter ("attack", 0.005f, 1);
        versionManager.registerParameter ("release", 0.01f, 1);
        versionManager.registerParameter ("gain", 0.8f, 1);
        versionManager.registerParameter ("mix", 1.0f, 1);


    }

    ~StutterGateProcessor() override
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
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "activePattern", 1 }, "Pattern Slot", juce::StringArray { "Pattern 1", "Pattern 2", "Pattern 3", "Pattern 4", "Pattern 5", "Pattern 6" }, 0));
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "subdivision", 1 }, "Subdivision", juce::StringArray { "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars" }, 3));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "attack", 1 }, "Attack", juce::NormalisableRange<float> (0.001f, 0.5f), 0.005f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "release", 1 }, "Release", juce::NormalisableRange<float> (0.001f, 0.5f), 0.01f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output Gain", juce::NormalisableRange<float> (0.0f, 2.0f), 0.8f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Wet/Dry Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = 1347570764u; // seed lock: reproducible renders




        sm_activePattern.reset (apvts.getRawParameterValue ("activePattern")->load(), 10.0f, sampleRate);
        sm_subdivision.reset (apvts.getRawParameterValue ("subdivision")->load(), 10.0f, sampleRate);
        sm_attack.reset (apvts.getRawParameterValue ("attack")->load(), 10.0f, sampleRate);
        sm_release.reset (apvts.getRawParameterValue ("release")->load(), 10.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);
        sm_mix.reset (apvts.getRawParameterValue ("mix")->load(), 5.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        sampleRate = (float) sampleRate;
phase = 0.0f;
env = 0.0f;
lastStep = -1;
gainSmooth.reset(sampleRate, 0.020);
gainSmooth.setCurrentAndTargetValue(0.8f);
mixSmooth.reset(sampleRate, 0.020);
mixSmooth.setCurrentAndTargetValue(1.0f);
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
        // ---- custom block: StutterGate (AI-generated) ----
float rawPattern = apvts.getRawParameterValue("activePattern")->load();
int activePattern = (int) juce::jlimit(0.0f, 5.0f, rawPattern);
float rawSubdiv = apvts.getRawParameterValue("subdivision")->load();
int subdivIdx = (int) juce::jlimit(0.0f, 7.0f, rawSubdiv);
float subdiv = subdivValues[subdivIdx];
float rawAttack = apvts.getRawParameterValue("attack")->load();
float attackSec = juce::jlimit(0.001f, 0.5f, rawAttack);
float rawRelease = apvts.getRawParameterValue("release")->load();
float releaseSec = juce::jlimit(0.001f, 0.5f, rawRelease);
float rawGain = apvts.getRawParameterValue("gain")->load();
gainSmooth.setTargetValue(juce::jlimit(0.0f, 2.0f, rawGain));
float rawMix = apvts.getRawParameterValue("mix")->load();
mixSmooth.setTargetValue(juce::jlimit(0.0f, 1.0f, rawMix));
float attackCoeff = 1.0f - std::exp(-1.0f / (attackSec * sampleRate));
float releaseCoeff = 1.0f - std::exp(-1.0f / (releaseSec * sampleRate));
float bpm = 120.0f;
if (auto* ph = getPlayHead()) {
  juce::AudioPlayHead::CurrentPositionInfo pos;
  if (ph->getCurrentPosition(pos)) bpm = (float) pos.bpm;
}
bpm = juce::jlimit(20.0f, 300.0f, bpm);
float phaseInc = (bpm / 60.0f) * subdiv / sampleRate;
size_t numCh = block.getNumChannels();
size_t numSamples = block.getNumSamples();
for (size_t i = 0; i < numSamples; ++i) {
  phase += phaseInc;
  if (phase >= 1.0f) phase -= 1.0f;
  int stepIndex = (int) (phase * NUM_STEPS);
  if (stepIndex >= NUM_STEPS) stepIndex = NUM_STEPS - 1;
  float target = (float) patterns[activePattern][stepIndex];
  if (target > env) env += (1.0f - env) * attackCoeff;
  else env += (0.0f - env) * releaseCoeff;
  float g = gainSmooth.getNextValue();
  float m = mixSmooth.getNextValue();
  for (size_t ch = 0; ch < numCh; ++ch) {
    auto* d = block.getChannelPointer((int) ch);
    float dry = d[i];
    d[i] = (dry * env * g * m) + (dry * (1.0f - m));
  }
}
        gainDsp.setGainDecibels (smoothedParam ("gain"));
    gainDsp.process (ctx);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ignoreUnused (midiMessages);
        juce::ScopedNoDenormals noDenormals;
        sm_activePattern.setTarget (apvts.getRawParameterValue ("activePattern")->load());
        sm_activePattern.update (getSampleRate());
        sm_subdivision.setTarget (apvts.getRawParameterValue ("subdivision")->load());
        sm_subdivision.update (getSampleRate());
        sm_attack.setTarget (apvts.getRawParameterValue ("attack")->load());
        sm_attack.update (getSampleRate());
        sm_release.setTarget (apvts.getRawParameterValue ("release")->load());
        sm_release.update (getSampleRate());
        sm_gain.setTarget (apvts.getRawParameterValue ("gain")->load());
        sm_gain.update (getSampleRate());
        sm_mix.setTarget (apvts.getRawParameterValue ("mix")->load());
        sm_mix.update (getSampleRate());
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
    const juce::String getName() const override { return "StutterGate"; }
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
    WoManusSmoothedParameter<float> sm_activePattern;
    WoManusSmoothedParameter<float> sm_subdivision;
    WoManusSmoothedParameter<float> sm_attack;
    WoManusSmoothedParameter<float> sm_release;
    WoManusSmoothedParameter<float> sm_gain;
    WoManusSmoothedParameter<float> sm_mix;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "activePattern") == 0) return sm_activePattern.getNextValue();
        if (strcmp (id, "subdivision") == 0) return sm_subdivision.getNextValue();
        if (strcmp (id, "attack") == 0) return sm_attack.getNextValue();
        if (strcmp (id, "release") == 0) return sm_release.getNextValue();
        if (strcmp (id, "gain") == 0) return sm_gain.getNextValue();
        if (strcmp (id, "mix") == 0) return sm_mix.getNextValue();
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
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("subdivision")) param->setValueNotifyingHost (param->convertTo0to1 (3.0f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (0.005f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (0.01f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.8f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("subdivision")) param->setValueNotifyingHost (param->convertTo0to1 (2.3649f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (0.1507f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (0.1507f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.7005f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("subdivision")) param->setValueNotifyingHost (param->convertTo0to1 (3.3314f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (0.1507f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (0.1507f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.9805f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("subdivision")) param->setValueNotifyingHost (param->convertTo0to1 (3.3326f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (0.1507f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (0.1507f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.7688f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("subdivision")) param->setValueNotifyingHost (param->convertTo0to1 (3.5959f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (0.1507f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (0.1507f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.8404f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("subdivision")) param->setValueNotifyingHost (param->convertTo0to1 (2.6118f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (0.1507f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (0.1507f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.898f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (4.2338f));
        if (auto* param = apvts.getParameter ("subdivision")) param->setValueNotifyingHost (param->convertTo0to1 (2.7057f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (0.2588f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.5434f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5136f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (1.8641f));
        if (auto* param = apvts.getParameter ("subdivision")) param->setValueNotifyingHost (param->convertTo0to1 (3.2207f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (0.0794f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (0.2062f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.2691f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4551f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (2.3529f));
        if (auto* param = apvts.getParameter ("subdivision")) param->setValueNotifyingHost (param->convertTo0to1 (1.0538f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (0.3887f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (0.1422f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.1548f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0525f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (3.2201f));
        if (auto* param = apvts.getParameter ("subdivision")) param->setValueNotifyingHost (param->convertTo0to1 (5.5135f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (0.3811f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (0.3348f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.1184f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.6275f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (4.2222f));
        if (auto* param = apvts.getParameter ("subdivision")) param->setValueNotifyingHost (param->convertTo0to1 (4.1455f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (0.4437f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (0.3323f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0204f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.8385f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (0.0681f));
        if (auto* param = apvts.getParameter ("subdivision")) param->setValueNotifyingHost (param->convertTo0to1 (6.5778f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (0.0102f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (0.0321f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0287f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.973f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (4.7004f));
        if (auto* param = apvts.getParameter ("subdivision")) param->setValueNotifyingHost (param->convertTo0to1 (6.5203f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (0.4608f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (0.0176f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.9016f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0537f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (4.7513f));
        if (auto* param = apvts.getParameter ("subdivision")) param->setValueNotifyingHost (param->convertTo0to1 (0.3618f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (0.0235f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (0.465f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0035f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9498f));
        break;
        default: break;
        }


    }


float phase = 0.0f;
float env = 0.0f;
int lastStep = -1;
float sampleRate = 44100.0f;
static constexpr int NUM_PATTERNS = 6;
static constexpr int NUM_STEPS = 16;
const int patterns[6][16] = {
  {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
  {1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0},
  {1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0},
  {1,1,1,0,1,1,1,0,1,1,1,0,1,1,1,0},
  {1,0,1,1,0,1,0,1,1,0,1,1,0,1,0,1},
  {1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,0}
};
const float subdivValues[8] = {0.125f,0.25f,0.5f,1.0f,2.0f,4.0f,8.0f,16.0f};
juce::SmoothedValue<float> gainSmooth;
juce::SmoothedValue<float> mixSmooth;
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StutterGateProcessor)
};
