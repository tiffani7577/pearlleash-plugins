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


class RhythmGateForgeProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    RhythmGateForgeProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, juce::Identifier ("PARAMS"), createLayout())
    {
        // Portable FTZ (x86 MXCSR / arm64 FPCR). processBlock also uses juce::ScopedNoDenormals.
        pfEnableFlushToZero();

        licensed = RhythmGateForgeLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("RhythmGateForge: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("patternSlot", 0.0f, 1);
        versionManager.registerParameter ("attack", 10.0f, 1);
        versionManager.registerParameter ("release", 10.0f, 1);
        versionManager.registerParameter ("mix", 1.0f, 1);
        versionManager.registerParameter ("gain", 1.0f, 1);


    }

    ~RhythmGateForgeProcessor() override
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
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "patternSlot", 1 }, "Pattern Slot", juce::StringArray { "Straight 16ths", "Dotted 8th", "Syncopated", "Triplet", "Half-Time", "Shuffle" }, 0));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "attack", 1 }, "Attack", juce::NormalisableRange<float> (1.0f, 500.0f), 10.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "release", 1 }, "Release", juce::NormalisableRange<float> (1.0f, 500.0f), 10.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output Gain", juce::NormalisableRange<float> (0.0f, 4.0f), 1.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = 1347570764u; // seed lock: reproducible renders




        sm_patternSlot.reset (apvts.getRawParameterValue ("patternSlot")->load(), 10.0f, sampleRate);
        sm_attack.reset (apvts.getRawParameterValue ("attack")->load(), 10.0f, sampleRate);
        sm_release.reset (apvts.getRawParameterValue ("release")->load(), 10.0f, sampleRate);
        sm_mix.reset (apvts.getRawParameterValue ("mix")->load(), 5.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        gateCoeff[0] = 0.0f; gateCoeff[1] = 0.0f;
prevGateTarget[0] = 0.0f; prevGateTarget[1] = 0.0f;
float sr = (float) sampleRate;
mixSmooth.reset(sr, 0.015);
mixSmooth.setCurrentAndTargetValue(1.0f);
gainSmooth.reset(sr, 0.015);
gainSmooth.setCurrentAndTargetValue(1.0f);
attackSmooth.reset(sr, 0.015);
attackSmooth.setCurrentAndTargetValue(10.0f);
releaseSmooth.reset(sr, 0.015);
releaseSmooth.setCurrentAndTargetValue(10.0f);
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
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
                if (auto playing = pos->getIsPlaying())
                    return *playing;
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
        // ---- custom block: RhythmGateForge (AI-generated) ----
float sr = (float)getSampleRate();
float rawPattern = apvts.getRawParameterValue("patternSlot")->load();
float rawAttack  = apvts.getRawParameterValue("attack")->load();
float rawRelease = apvts.getRawParameterValue("release")->load();
float rawMix     = apvts.getRawParameterValue("mix")->load();
float rawGain    = apvts.getRawParameterValue("gain")->load();
int patSlot = (int)juce::jlimit(0.0f, 5.0f, rawPattern);
attackSmooth.setTargetValue(juce::jlimit(1.0f, 500.0f, rawAttack));
releaseSmooth.setTargetValue(juce::jlimit(1.0f, 500.0f, rawRelease));
mixSmooth.setTargetValue(juce::jlimit(0.0f, 1.0f, rawMix));
gainSmooth.setTargetValue(juce::jlimit(0.0f, 4.0f, rawGain));
float bpm = 120.0f;
auto* ph = getPlayHead();
juce::AudioPlayHead::CurrentPositionInfo pos;
bool hasPos = false;
double ppqPos = 0.0;
if (ph && ph->getCurrentPosition(pos)) { bpm = (float)pos.bpm; ppqPos = pos.ppqPosition; hasPos = true; }
int numSamples = block.getNumSamples();
int numChannels = block.getNumChannels();
for (size_t i = 0; i < (size_t) numSamples; ++i) {
  float attackMs  = attackSmooth.getNextValue();
  float releaseMs = releaseSmooth.getNextValue();
  float mix  = mixSmooth.getNextValue();
  float gain = gainSmooth.getNextValue();
  float attackSamples  = attackMs  * sr / 1000.0f;
  float releaseSamples = releaseMs * sr / 1000.0f;
  float attackCoeff  = 1.0f / (attackSamples  + 1.0f);
  float releaseCoeff = 1.0f / (releaseSamples + 1.0f);
  double samplePpq = ppqPos + (double)i * (bpm / 60.0) / sr;
  double beatPos = samplePpq;
  int stepIndex = (int)(beatPos * 4.0) % NUM_STEPS;
  if (stepIndex < 0) stepIndex = 0;
  float gateTarget = (float)patterns[patSlot][stepIndex];
  for (size_t ch = 0; ch < (size_t) numChannels && ch < 2; ++ch) {
    float* data = block.getChannelPointer((int)ch);
    float input = data[i];
    if (gateTarget >= 0.5f)
      gateCoeff[ch] += (1.0f - gateCoeff[ch]) * attackCoeff;
    else
      gateCoeff[ch] += (0.0f - gateCoeff[ch]) * releaseCoeff;
    float gated = input * gateCoeff[ch];
    data[i] = (input * (1.0f - mix) + gated * mix) * gain;
  }
}
        gainDsp.setGainDecibels (smoothedParam ("gain"));
    gainDsp.process (ctx);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ignoreUnused (midiMessages);
        juce::ScopedNoDenormals noDenormals;
        sm_patternSlot.setTarget (apvts.getRawParameterValue ("patternSlot")->load());
        sm_patternSlot.update (getSampleRate());
        sm_attack.setTarget (apvts.getRawParameterValue ("attack")->load());
        sm_attack.update (getSampleRate());
        sm_release.setTarget (apvts.getRawParameterValue ("release")->load());
        sm_release.update (getSampleRate());
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
    const juce::String getName() const override { return "RhythmGateForge"; }
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
    WoManusSmoothedParameter<float> sm_patternSlot;
    WoManusSmoothedParameter<float> sm_attack;
    WoManusSmoothedParameter<float> sm_release;
    WoManusSmoothedParameter<float> sm_mix;
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "patternSlot") == 0) return sm_patternSlot.getNextValue();
        if (strcmp (id, "attack") == 0) return sm_attack.getNextValue();
        if (strcmp (id, "release") == 0) return sm_release.getNextValue();
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
        if (auto* param = apvts.getParameter ("patternSlot")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (10.0f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (10.0f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("patternSlot")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (150.7f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (150.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.2f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("patternSlot")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (150.7f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (150.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.2f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("patternSlot")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (150.7f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (150.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.2f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("patternSlot")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (150.7f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (150.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.2f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("patternSlot")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (150.7f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (150.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.2f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("patternSlot")) param->setValueNotifyingHost (param->convertTo0to1 (4.3534f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (362.2082f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (498.0745f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.6515f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (2.8224f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("patternSlot")) param->setValueNotifyingHost (param->convertTo0to1 (3.1907f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (321.5894f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (233.1509f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.2068f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.5465f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("patternSlot")) param->setValueNotifyingHost (param->convertTo0to1 (3.6203f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (199.5874f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (303.1127f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4142f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (2.6861f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("patternSlot")) param->setValueNotifyingHost (param->convertTo0to1 (3.128f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (184.988f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (194.6004f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.6822f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (3.043f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("patternSlot")) param->setValueNotifyingHost (param->convertTo0to1 (4.8627f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (412.2661f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (276.0321f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.769f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (2.6877f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("patternSlot")) param->setValueNotifyingHost (param->convertTo0to1 (4.958f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (482.2299f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (14.2475f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9532f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.2282f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("patternSlot")) param->setValueNotifyingHost (param->convertTo0to1 (4.6406f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (5.8815f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (473.1325f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9897f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (3.775f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("patternSlot")) param->setValueNotifyingHost (param->convertTo0to1 (4.8359f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (464.8291f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (488.5639f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9286f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.1397f));
        break;
        default: break;
        }


    }


float gateCoeff[2];
float prevGateTarget[2];
juce::SmoothedValue<float> mixSmooth;
juce::SmoothedValue<float> gainSmooth;
juce::SmoothedValue<float> attackSmooth;
juce::SmoothedValue<float> releaseSmooth;
static constexpr int NUM_STEPS = 16;
static constexpr int NUM_PATTERNS = 6;
const int patterns[NUM_PATTERNS][NUM_STEPS] = {
  {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
  {1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1},
  {1,0,1,0,0,0,1,0,1,0,1,0,0,0,1,0},
  {1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1},
  {1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0},
  {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0}
};
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RhythmGateForgeProcessor)
};
