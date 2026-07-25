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


class RhythmChopprProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    RhythmChopprProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, juce::Identifier ("PARAMS"), createLayout())
    {
        // Portable FTZ (x86 MXCSR / arm64 FPCR). processBlock also uses juce::ScopedNoDenormals.
        pfEnableFlushToZero();

        licensed = RhythmChopprLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("RhythmChoppr: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("activePattern", 0.0f, 1);
        versionManager.registerParameter ("attack", 5.0f, 1);
        versionManager.registerParameter ("release", 5.0f, 1);
        versionManager.registerParameter ("mix", 0.8f, 1);
        versionManager.registerParameter ("gain", 1.0f, 1);


    }

    ~RhythmChopprProcessor() override
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
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "attack", 1 }, "Attack", juce::NormalisableRange<float> (1.0f, 50.0f), 5.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "release", 1 }, "Release", juce::NormalisableRange<float> (1.0f, 50.0f), 5.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Wet/Dry Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 0.8f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output Gain", juce::NormalisableRange<float> (0.0f, 2.0f), 1.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = 1347570764u; // seed lock: reproducible renders




        sm_activePattern.reset (apvts.getRawParameterValue ("activePattern")->load(), 10.0f, sampleRate);
        sm_attack.reset (apvts.getRawParameterValue ("attack")->load(), 10.0f, sampleRate);
        sm_release.reset (apvts.getRawParameterValue ("release")->load(), 10.0f, sampleRate);
        sm_mix.reset (apvts.getRawParameterValue ("mix")->load(), 5.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        float sr = (float) sampleRate;
mixSmooth.reset(sr, 0.020);
mixSmooth.setCurrentAndTargetValue(0.8f);
gainSmooth.reset(sr, 0.020);
gainSmooth.setCurrentAndTargetValue(1.0f);
attackSmooth.reset(sr, 0.020);
attackSmooth.setCurrentAndTargetValue(5.0f);
releaseSmooth.reset(sr, 0.020);
releaseSmooth.setCurrentAndTargetValue(5.0f);
envelope = 0.0f;
currentTarget = 0.0f;
float attackMs = 5.0f;
float releaseMs = 5.0f;
attackCoeff = std::exp(-1.0f / ((attackMs * 0.001f) * sr));
releaseCoeff = std::exp(-1.0f / ((releaseMs * 0.001f) * sr));
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
        // ---- custom block: RhythmChoppr (AI-generated) ----
float rawPattern = apvts.getRawParameterValue("activePattern")->load();
int activePattern = (int) juce::jlimit(0.0f, 5.0f, rawPattern);
float rawMix = apvts.getRawParameterValue("mix")->load();
float clampedMix = juce::jlimit(0.0f, 1.0f, rawMix);
mixSmooth.setTargetValue(clampedMix);
float rawGain = apvts.getRawParameterValue("gain")->load();
float clampedGain = juce::jlimit(0.0f, 2.0f, rawGain);
gainSmooth.setTargetValue(clampedGain);
float rawAttack = apvts.getRawParameterValue("attack")->load();
float clampedAttack = juce::jlimit(1.0f, 50.0f, rawAttack);
attackSmooth.setTargetValue(clampedAttack);
float rawRelease = apvts.getRawParameterValue("release")->load();
float clampedRelease = juce::jlimit(1.0f, 50.0f, rawRelease);
releaseSmooth.setTargetValue(clampedRelease);
float sr = (float) getSampleRate();
double bpm = getBpm();
if (bpm <= 0.0) bpm = 120.0;
double beatsPerBar = 4.0;
size_t numSamples = block.getNumSamples();
size_t numChannels = block.getNumChannels();
for (size_t i = 0; i < numSamples; ++i) {
  float attackMs = attackSmooth.getNextValue();
  float releaseMs = releaseSmooth.getNextValue();
  attackCoeff = std::exp(-1.0f / ((attackMs * 0.001f) * sr));
  releaseCoeff = std::exp(-1.0f / ((releaseMs * 0.001f) * sr));
  double beatPos = getPlayHeadBeatPosition();
  double barPhase = std::fmod(beatPos, beatsPerBar) / beatsPerBar;
  if (barPhase < 0.0) barPhase += 1.0;
  int stepIndex = (int) std::floor(barPhase * NUM_STEPS) % NUM_STEPS;
  float target = patterns[activePattern][stepIndex] ? 1.0f : 0.0f;
  float coeff = (target > currentTarget) ? attackCoeff : releaseCoeff;
  envelope = envelope * coeff + target * (1.0f - coeff);
  currentTarget = target;
  float mixVal = mixSmooth.getNextValue();
  float gainVal = gainSmooth.getNextValue();
  for (size_t ch = 0; ch < numChannels; ++ch) {
    auto* d = block.getChannelPointer((int) ch);
    float dry = d[i];
    float wet = dry * envelope;
    d[i] = (dry * (1.0f - mixVal) + wet * mixVal) * gainVal;
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
    const juce::String getName() const override { return "RhythmChoppr"; }
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
    WoManusSmoothedParameter<float> sm_attack;
    WoManusSmoothedParameter<float> sm_release;
    WoManusSmoothedParameter<float> sm_mix;
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "activePattern") == 0) return sm_activePattern.getNextValue();
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
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (5.0f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (5.0f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.8f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (15.7f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (15.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.0739f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (15.7f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (15.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.116f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (15.7f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (15.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.8013f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (15.7f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (15.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.1584f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (15.7f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (15.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.0054f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (3.1749f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (40.3254f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (18.984f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9912f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.0117f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (0.8849f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (14.393f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (16.8872f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.6448f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.3064f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (2.8056f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (31.0736f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (15.8644f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.6351f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.1456f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (3.5971f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (21.8457f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (28.171f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4032f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.3545f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (3.6882f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (35.084f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (11.8845f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.3928f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.4434f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (4.849f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (48.5755f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (1.6724f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9337f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0771f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (0.2411f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (1.511f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (3.6404f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0181f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.8896f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("activePattern")) param->setValueNotifyingHost (param->convertTo0to1 (4.6666f));
        if (auto* param = apvts.getParameter ("attack")) param->setValueNotifyingHost (param->convertTo0to1 (48.427f));
        if (auto* param = apvts.getParameter ("release")) param->setValueNotifyingHost (param->convertTo0to1 (1.9852f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9623f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.101f));
        break;
        default: break;
        }


    }


static constexpr int NUM_PATTERNS = 6;
static constexpr int NUM_STEPS = 16;
bool patterns[NUM_PATTERNS][NUM_STEPS] = {
  {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
  {1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0},
  {1,1,1,0,1,1,1,0,1,1,1,0,1,1,1,0},
  {1,0,0,1,0,0,1,0,1,0,0,1,0,0,1,0},
  {1,1,1,1,0,0,0,0,1,1,1,1,0,0,0,0},
  {1,0,1,1,0,1,0,1,1,0,1,0,1,1,0,1}
};
float envelope = 0.0f;
float smoothCoeff = 0.99f;
juce::SmoothedValue<float> mixSmooth;
juce::SmoothedValue<float> gainSmooth;
juce::SmoothedValue<float> attackSmooth;
juce::SmoothedValue<float> releaseSmooth;
float currentTarget = 0.0f;
float attackCoeff = 0.99f;
float releaseCoeff = 0.99f;
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RhythmChopprProcessor)
};
