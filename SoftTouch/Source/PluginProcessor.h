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
        versionManager.registerParameter ("mix", 1.0f, 1);
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
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));
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
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        srF = (float) sampleRate;
driveSmooth.reset(srF, 0.020f);
driveSmooth.setCurrentAndTargetValue(0.3f);
warmthSmooth.reset(srF, 0.020f);
warmthSmooth.setCurrentAndTargetValue(0.5f);
presenceSmooth.reset(srF, 0.020f);
presenceSmooth.setCurrentAndTargetValue(0.5f);
mixSmoothed.reset(srF, 0.020f);
mixSmoothed.setCurrentAndTargetValue(1.0f);
for (size_t c = 0; c < (size_t) 2; ++c) { ic1eq[c] = 0.0f; ic2eq[c] = 0.0f; ic3eq[c] = 0.0f; ic4eq[c] = 0.0f; }
for (size_t k = 0; k < (size_t) 4; ++k) { os_state[k] = 0.0f; ds_state[k] = 0.0f; }
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
const float rawMix = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("mix")->load());
driveSmooth.setTargetValue(rawDrive);
warmthSmooth.setTargetValue(rawWarmth);
presenceSmooth.setTargetValue(rawPresence);
mixSmoothed.setTargetValue(rawMix);
const float osRate = srF * 4.0f;
for (size_t ch = 0; ch < block.getNumChannels(); ++ch) {
  auto* d = block.getChannelPointer((int)ch);
  int ci = (int)ch < 2 ? (int)ch : 0;
  for (size_t i = 0; i < block.getNumSamples(); ++i) {
    float driveSm = driveSmooth.getNextValue();
    float warmSm = warmthSmooth.getNextValue();
    float presSm = presenceSmooth.getNextValue();
    float internalDrive = 0.5f + driveSm * 7.5f;
    float dry = d[i];
    float x = dry;
    float osOut = 0.0f;
    float normD = tanhf(internalDrive);
    if (normD < 1e-6f) normD = 1e-6f;
    for (size_t k = 0; k < (size_t) 4; ++k) {
      float xUp = (k == 0) ? x : 0.0f;
      float g_up = tanf(3.14159265f * 18000.0f / osRate);
      float k_up = 0.7072f;
      float a1u = 1.0f / (1.0f + g_up * (g_up + k_up));
      float v1u = a1u * (xUp - ic2eq[ci] - k_up * ic1eq[ci]);
      float v2u = ic2eq[ci] + g_up * v1u;
      ic1eq[ci] = 2.0f * v1u - ic1eq[ci];
      ic2eq[ci] = 2.0f * v2u - ic2eq[ci];
      float sat = tanhf(internalDrive * v2u) / normD;
      osOut += sat;
    }
    osOut *= 0.25f;
    float g_ds = tanf(3.14159265f * 18000.0f / osRate);
    float k_ds = 0.7072f;
    float a1d = 1.0f / (1.0f + g_ds * (g_ds + k_ds));
    float v1d = a1d * (osOut - ic3eq[ci] - k_ds * ic4eq[ci]);
    float v2d = ic3eq[ci] + g_ds * v1d;
    ic3eq[ci] = 2.0f * v1d - ic3eq[ci];
    ic3eq[ci] = ic3eq[ci];
    ic4eq[ci] = 2.0f * v2d - ic4eq[ci];
    float filtered = v2d;
    float warmCutoff = 80.0f + warmSm * 320.0f;
    float gW = tanf(3.14159265f * warmCutoff / srF);
    float warmGainLin = 1.0f + warmSm * (powf(10.0f, 3.0f / 20.0f) - 1.0f);
    float lsOut = filtered + (warmGainLin - 1.0f) * gW / (1.0f + gW) * filtered;
    float presCutoff = 6000.0f;
    float gP = tanf(3.14159265f * presCutoff / srF);
    float presGainLin = 1.0f + presSm * (powf(10.0f, 4.0f / 20.0f) - 1.0f);
    float hsOut = lsOut + (presGainLin - 1.0f) * (1.0f - gP / (1.0f + gP)) * lsOut;
    float makeup = internalDrive / normD;
    if (makeup > 4.0f) makeup = 4.0f;
    float wet = hsOut / makeup;
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
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "drive") == 0) return sm_drive.getNextValue();
        if (strcmp (id, "warmth") == 0) return sm_warmth.getNextValue();
        if (strcmp (id, "presence") == 0) return sm_presence.getNextValue();
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
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3009f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.4041f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.554f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.9076f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3067f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.4431f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.5976f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (3.7522f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.5247f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.4793f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-4.0012f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.5307f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.543f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-1.9744f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3402f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.5101f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.5159f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.6677f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.6261f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.9174f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.8404f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (24.0f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.6315f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.1379f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.459f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5831f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (7.2039f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.8413f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.5001f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.6629f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.3096f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-11.7516f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.533f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.528f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.3766f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4273f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (13.1675f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.7555f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7779f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.5302f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.3356f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (8.5706f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.9877f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.0418f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.074f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9911f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-23.0268f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.9824f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.9459f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.9606f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0169f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-21.2389f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.0727f));
        if (auto* param = apvts.getParameter ("warmth")) param->setValueNotifyingHost (param->convertTo0to1 (0.0745f));
        if (auto* param = apvts.getParameter ("presence")) param->setValueNotifyingHost (param->convertTo0to1 (0.0525f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9276f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (22.4906f));
        break;
        default: break;
        }


    }


juce::SmoothedValue<float> driveSmooth;
juce::SmoothedValue<float> warmthSmooth;
juce::SmoothedValue<float> presenceSmooth;
juce::SmoothedValue<float> mixSmoothed;
float ic1eq[2] = {0.0f, 0.0f};
float ic2eq[2] = {0.0f, 0.0f};
float ic3eq[2] = {0.0f, 0.0f};
float ic4eq[2] = {0.0f, 0.0f};
float srF = 44100.0f;
float os_buf[8] = {0.0f};
float os_state[4] = {0.0f};
float ds_state[4] = {0.0f};
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoftTouchProcessor)
};
