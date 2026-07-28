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
        versionManager.registerParameter ("drive", 0.3f, 1);
        versionManager.registerParameter ("warmth", 0.5f, 1);
        versionManager.registerParameter ("presence", 0.5f, 1);
        versionManager.registerParameter ("mix", 0.5f, 1);
        versionManager.registerParameter ("output", 0.0f, 1);
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
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "drive", 1 }, "Drive", juce::NormalisableRange<float> (0.0f, 1.0f), 0.3f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "warmth", 1 }, "Warmth", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "presence", 1 }, "Presence", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "output", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 12.0f), 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = 1347570764u; // seed lock: reproducible renders




        sm_drive.reset (apvts.getRawParameterValue ("drive")->load(), 10.0f, sampleRate);
        sm_warmth.reset (apvts.getRawParameterValue ("warmth")->load(), 10.0f, sampleRate);
        sm_presence.reset (apvts.getRawParameterValue ("presence")->load(), 10.0f, sampleRate);
        sm_mix.reset (apvts.getRawParameterValue ("mix")->load(), 5.0f, sampleRate);
        sm_output.reset (apvts.getRawParameterValue ("output")->load(), 10.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        srF = (float) sampleRate;
driveSmoothed.reset(srF, 0.020);
driveSmoothed.setCurrentAndTargetValue(0.3f);
warmthSmoothed.reset(srF, 0.020);
warmthSmoothed.setCurrentAndTargetValue(0.5f);
presenceSmoothed.reset(srF, 0.020);
presenceSmoothed.setCurrentAndTargetValue(0.5f);
mixSmoothed.reset(srF, 0.020);
mixSmoothed.setCurrentAndTargetValue(0.5f);
outputSmoothed.reset(srF, 0.020);
outputSmoothed.setCurrentAndTargetValue(1.0f);
for (size_t c = 0; c < (size_t) 2; ++c){warmthZ1[c]=0.0f;warmthZ2[c]=0.0f;presZ1[c]=0.0f;presZ2[c]=0.0f;}
calcWarmth(2.0f);
calcPresence(1.5f);
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
{
float rawDrive = juce::jlimit(0.0f,1.0f,apvts.getRawParameterValue("drive")->load());
float rawWarmth = juce::jlimit(0.0f,1.0f,apvts.getRawParameterValue("warmth")->load());
float rawPresence = juce::jlimit(0.0f,1.0f,apvts.getRawParameterValue("presence")->load());
float rawMix = juce::jlimit(0.0f,1.0f,apvts.getRawParameterValue("mix")->load());
float rawOutput = juce::jlimit(-24.0f,12.0f,apvts.getRawParameterValue("output")->load());
driveSmoothed.setTargetValue(rawDrive);
warmthSmoothed.setTargetValue(rawWarmth);
presenceSmoothed.setTargetValue(rawPresence);
mixSmoothed.setTargetValue(rawMix);
outputSmoothed.setTargetValue(powf(10.0f, rawOutput/20.0f));
calcWarmth(rawWarmth * 4.0f);
calcPresence(rawPresence * 3.0f);
const size_t numSamples = block.getNumSamples();
const size_t numChannels = block.getNumChannels();
for (size_t i = 0; i < numSamples; ++i) {
  float drv = driveSmoothed.getNextValue();
  float outG = outputSmoothed.getNextValue();
  float m = mixSmoothed.getNextValue();
  float dScaled = 1.0f + drv * 7.0f;
  float norm = tanhf(dScaled);
  if(norm < 1e-6f) norm = 1e-6f;
  for (size_t ch = 0; ch < numChannels; ++ch) {
    auto* d = block.getChannelPointer((int)ch);
    int chIdx = (int)ch < 2 ? (int)ch : 1;
    float dry = d[i];
    float xw = dry;
    float yw = wB0*xw + warmthZ1[chIdx];
    warmthZ1[chIdx] = wB1*xw - wA1*yw + warmthZ2[chIdx];
    warmthZ2[chIdx] = wB2*xw - wA2*yw;
    float xSat = yw;
    float wet = tanhf(dScaled * xSat) / norm;
    float xp = wet;
    float yp = pB0*xp + presZ1[chIdx];
    presZ1[chIdx] = pB1*xp - pA1*yp + presZ2[chIdx];
    presZ2[chIdx] = pB2*xp - pA2*yp;
    float processedSample = yp;
    float output = dry * (1.0f - m) + processedSample * m;
    d[i] = output * outG;
  }
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
        sm_mix.setTarget (apvts.getRawParameterValue ("mix")->load());
        sm_mix.update (getSampleRate());
        sm_output.setTarget (apvts.getRawParameterValue ("output")->load());
        sm_output.update (getSampleRate());
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
    WoManusSmoothedParameter<float> sm_mix;
    WoManusSmoothedParameter<float> sm_output;
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "drive") == 0) return sm_drive.getNextValue();
        if (strcmp (id, "warmth") == 0) return sm_warmth.getNextValue();
        if (strcmp (id, "presence") == 0) return sm_presence.getNextValue();
        if (strcmp (id, "mix") == 0) return sm_mix.getNextValue();
        if (strcmp (id, "output") == 0) return sm_output.getNextValue();
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
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("output")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3009f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.4041f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.554f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4615f));
        if (auto* param = apvts.getParameter ("output")) param->setValueNotifyingHost (param->convertTo0to1 (0.6807f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.3216f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.5976f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.5552f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5782f));
        if (auto* param = apvts.getParameter ("output")) param->setValueNotifyingHost (param->convertTo0to1 (-0.5115f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.1856f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.476f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.4166f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4285f));
        if (auto* param = apvts.getParameter ("output")) param->setValueNotifyingHost (param->convertTo0to1 (1.1068f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (2.0629f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.4589f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.5402f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5101f));
        if (auto* param = apvts.getParameter ("output")) param->setValueNotifyingHost (param->convertTo0to1 (0.5719f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.237f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3139f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.4952f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.5535f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5381f));
        if (auto* param = apvts.getParameter ("output")) param->setValueNotifyingHost (param->convertTo0to1 (1.2f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (4.3344f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.2131f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.7483f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9551f));
        if (auto* param = apvts.getParameter ("output")) param->setValueNotifyingHost (param->convertTo0to1 (12.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (24.0f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.4376f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.5597f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.2947f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.2539f));
        if (auto* param = apvts.getParameter ("output")) param->setValueNotifyingHost (param->convertTo0to1 (-10.3331f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-6.0676f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.1726f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.2537f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.8089f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.6456f));
        if (auto* param = apvts.getParameter ("output")) param->setValueNotifyingHost (param->convertTo0to1 (-8.1973f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (15.6395f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.4049f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.512f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.7782f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7234f));
        if (auto* param = apvts.getParameter ("output")) param->setValueNotifyingHost (param->convertTo0to1 (-8.2498f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (2.9488f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.0689f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.5744f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.5573f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.6107f));
        if (auto* param = apvts.getParameter ("output")) param->setValueNotifyingHost (param->convertTo0to1 (-14.3613f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-4.5367f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.9824f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.9459f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.9606f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0169f));
        if (auto* param = apvts.getParameter ("output")) param->setValueNotifyingHost (param->convertTo0to1 (-21.9291f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-20.5123f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.0745f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.0525f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.9276f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9686f));
        if (auto* param = apvts.getParameter ("output")) param->setValueNotifyingHost (param->convertTo0to1 (-21.3244f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-23.9026f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.0343f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.9916f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.9973f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0683f));
        if (auto* param = apvts.getParameter ("output")) param->setValueNotifyingHost (param->convertTo0to1 (-22.8118f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (22.8572f));
        break;
        default: break;
        }


    }


juce::SmoothedValue<float> driveSmoothed;
juce::SmoothedValue<float> warmthSmoothed;
juce::SmoothedValue<float> presenceSmoothed;
juce::SmoothedValue<float> mixSmoothed;
juce::SmoothedValue<float> outputSmoothed;
float srF = 44100.0f;
float warmthZ1[2] = {0.0f, 0.0f};
float warmthZ2[2] = {0.0f, 0.0f};
float presZ1[2] = {0.0f, 0.0f};
float presZ2[2] = {0.0f, 0.0f};
float wB0=1,wB1=0,wB2=0,wA1=0,wA2=0;
float pB0=1,pB1=0,pB2=0,pA1=0,pA2=0;
void calcWarmth(float gainDb){
  float fc=250.0f;
  float A=powf(10.0f,gainDb/40.0f);
  float w0=2.0f*3.14159265f*fc/srF;
  float cosw=cosf(w0),sinw=sinf(w0);
  float alpha=sinw/2.0f*sqrtf((A+1.0f/A)*(1.0f-1.0f)+2.0f);
  alpha = sinw * 0.7071f;
  float a0=(A+1.0f)+(A-1.0f)*cosw+2.0f*sqrtf(A)*alpha;
  wB0=A*((A+1.0f)-(A-1.0f)*cosw+2.0f*sqrtf(A)*alpha)/a0;
  wB1=2.0f*A*((A-1.0f)-(A+1.0f)*cosw)/a0;
  wB2=A*((A+1.0f)-(A-1.0f)*cosw-2.0f*sqrtf(A)*alpha)/a0;
  wA1=-2.0f*((A-1.0f)+(A+1.0f)*cosw)/a0;
  wA2=((A+1.0f)+(A-1.0f)*cosw-2.0f*sqrtf(A)*alpha)/a0;
}
void calcPresence(float gainDb){
  float fc=8000.0f;
  float A=powf(10.0f,gainDb/40.0f);
  float w0=2.0f*3.14159265f*fc/srF;
  float cosw=cosf(w0),sinw=sinf(w0);
  float alpha=sinw*0.7071f;
  float a0=(A+1.0f)-(A-1.0f)*cosw+2.0f*sqrtf(A)*alpha;
  pB0=A*((A+1.0f)+(A-1.0f)*cosw+2.0f*sqrtf(A)*alpha)/a0;
  pB1=-2.0f*A*((A-1.0f)+(A+1.0f)*cosw)/a0;
  pB2=A*((A+1.0f)+(A-1.0f)*cosw-2.0f*sqrtf(A)*alpha)/a0;
  pA1=2.0f*((A-1.0f)-(A+1.0f)*cosw)/a0;
  pA2=((A+1.0f)-(A-1.0f)*cosw-2.0f*sqrtf(A)*alpha)/a0;
}
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoftTouchProcessor)
};
