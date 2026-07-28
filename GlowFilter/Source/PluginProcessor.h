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


class GlowFilterProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    GlowFilterProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, juce::Identifier ("PARAMS"), createLayout())
    {
        // Portable FTZ (x86 MXCSR / arm64 FPCR). processBlock also uses juce::ScopedNoDenormals.
        pfEnableFlushToZero();

        licensed = GlowFilterLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("GlowFilter: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("cutoff", 800.0f, 1);
        versionManager.registerParameter ("resonance", 0.5f, 1);
        versionManager.registerParameter ("predrive", 1.5f, 1);
        versionManager.registerParameter ("gain", 1.0f, 1);
        versionManager.registerParameter ("mix", 1.0f, 1);


    }

    ~GlowFilterProcessor() override
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
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "cutoff", 1 }, "Cutoff", juce::NormalisableRange<float> (20.0f, 18000.0f), 800.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "resonance", 1 }, "Resonance", juce::NormalisableRange<float> (0.0f, 0.99f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "predrive", 1 }, "Drive", juce::NormalisableRange<float> (0.1f, 10.0f), 1.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output Gain", juce::NormalisableRange<float> (0.0f, 4.0f), 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = 1347570764u; // seed lock: reproducible renders




        sm_cutoff.reset (apvts.getRawParameterValue ("cutoff")->load(), 30.0f, sampleRate);
        sm_resonance.reset (apvts.getRawParameterValue ("resonance")->load(), 10.0f, sampleRate);
        sm_predrive.reset (apvts.getRawParameterValue ("predrive")->load(), 10.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);
        sm_mix.reset (apvts.getRawParameterValue ("mix")->load(), 5.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        srF = (float) sampleRate;
cutoffSmooth.reset(srF, 0.020);
cutoffSmooth.setCurrentAndTargetValue(800.0f);
resonanceSmooth.reset(srF, 0.020);
resonanceSmooth.setCurrentAndTargetValue(0.5f);
predriveSmooth.reset(srF, 0.020);
predriveSmooth.setCurrentAndTargetValue(1.5f);
gainSmooth.reset(srF, 0.020);
gainSmooth.setCurrentAndTargetValue(1.0f);
mixSmoothed.reset(srF, 0.020);
mixSmoothed.setCurrentAndTargetValue(1.0f);
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
        // ---- custom block: GlowFilter (AI-generated) ----
const float bpm = juce::jmax(1.0f, getBpm());
(void)bpm;
float rawCutoff = apvts.getRawParameterValue("cutoff")->load();
float rawRes    = apvts.getRawParameterValue("resonance")->load();
float rawPre    = apvts.getRawParameterValue("predrive")->load();
float rawGain   = apvts.getRawParameterValue("gain")->load();
float rawMix    = apvts.getRawParameterValue("mix")->load();
cutoffSmooth.setTargetValue(juce::jlimit(20.0f, 18000.0f, rawCutoff));
resonanceSmooth.setTargetValue(juce::jlimit(0.0f, 0.99f, rawRes));
predriveSmooth.setTargetValue(juce::jlimit(0.1f, 10.0f, rawPre));
gainSmooth.setTargetValue(juce::jlimit(0.0f, 4.0f, rawGain));
mixSmoothed.setTargetValue(juce::jlimit(0.0f, 1.0f, rawMix));
for (size_t ch = 0; ch < block.getNumChannels(); ++ch) {
  auto* d = block.getChannelPointer((int)ch);
  int c = (int)ch < 2 ? (int)ch : 1;
  for (size_t i = 0; i < block.getNumSamples(); ++i) {
    float fc   = cutoffSmooth.getNextValue();
    float res  = resonanceSmooth.getNextValue();
    float pre  = predriveSmooth.getNextValue();
    float gout = gainSmooth.getNextValue();
    float g = tanf(3.14159265f * juce::jlimit(20.0f, srF * 0.499f, fc) / srF);
    float k = juce::jlimit(0.02f, 2.0f, 2.0f - 2.0f * res);
    float inputSample = d[i];
    float xin = tubeSat(inputSample * pre, pre) ;
    float v0 = (xin - k * ic2eq[c]) / (1.0f + g * (g + k));
    float v1 = g * v0 + ic1eq[c];
    float v2 = g * v1 + ic2eq[c];
    ic1eq[c] = 2.0f * v1 - ic1eq[c];
    ic2eq[c] = tubeSat(2.0f * v2 - ic2eq[c], pre);
    float lp = v2;
    float bp = v1;
    float filtered = lp * 0.7f + bp * 0.3f;
    float processedSample = filtered * gout;
    float dry = inputSample;
    float wet = processedSample;
    d[i] = dry * (1.0f - mixSmoothed.getNextValue()) + wet * mixSmoothed.getNextValue();
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
        sm_predrive.setTarget (apvts.getRawParameterValue ("predrive")->load());
        sm_predrive.update (getSampleRate());
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
    const juce::String getName() const override { return "GlowFilter"; }
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
    WoManusSmoothedParameter<float> sm_predrive;
    WoManusSmoothedParameter<float> sm_gain;
    WoManusSmoothedParameter<float> sm_mix;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "cutoff") == 0) return sm_cutoff.getNextValue();
        if (strcmp (id, "resonance") == 0) return sm_resonance.getNextValue();
        if (strcmp (id, "predrive") == 0) return sm_predrive.getNextValue();
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
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (800.0f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("predrive")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (5414.0f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.4939f));
        if (auto* param = apvts.getParameter ("predrive")) param->setValueNotifyingHost (param->convertTo0to1 (3.07f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.2877f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (5414.0f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.5391f));
        if (auto* param = apvts.getParameter ("predrive")) param->setValueNotifyingHost (param->convertTo0to1 (3.07f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.2f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (5414.0f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.4534f));
        if (auto* param = apvts.getParameter ("predrive")) param->setValueNotifyingHost (param->convertTo0to1 (3.07f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.2f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (5414.0f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.5239f));
        if (auto* param = apvts.getParameter ("predrive")) param->setValueNotifyingHost (param->convertTo0to1 (3.07f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.3554f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (5414.0f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.4819f));
        if (auto* param = apvts.getParameter ("predrive")) param->setValueNotifyingHost (param->convertTo0to1 (3.07f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.2491f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (18000.0f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.8758f));
        if (auto* param = apvts.getParameter ("predrive")) param->setValueNotifyingHost (param->convertTo0to1 (1.8951f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (3.5988f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4239f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (4054.6213f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.1282f));
        if (auto* param = apvts.getParameter ("predrive")) param->setValueNotifyingHost (param->convertTo0to1 (5.6594f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.297f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.6641f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (2637.5854f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.6207f));
        if (auto* param = apvts.getParameter ("predrive")) param->setValueNotifyingHost (param->convertTo0to1 (8.3034f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.3935f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.8133f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (7103.5515f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.7266f));
        if (auto* param = apvts.getParameter ("predrive")) param->setValueNotifyingHost (param->convertTo0to1 (7.6062f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (3.1883f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.3447f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (12114.5821f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.239f));
        if (auto* param = apvts.getParameter ("predrive")) param->setValueNotifyingHost (param->convertTo0to1 (5.9264f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (2.8645f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5968f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (17965.3128f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.9146f));
        if (auto* param = apvts.getParameter ("predrive")) param->setValueNotifyingHost (param->convertTo0to1 (0.3719f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (3.7205f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9264f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (17908.5925f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.0048f));
        if (auto* param = apvts.getParameter ("predrive")) param->setValueNotifyingHost (param->convertTo0to1 (0.8851f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (3.9726f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.028f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("cutoff")) param->setValueNotifyingHost (param->convertTo0to1 (17874.2168f));
        if (auto* param = apvts.getParameter ("resonance")) param->setValueNotifyingHost (param->convertTo0to1 (0.0052f));
        if (auto* param = apvts.getParameter ("predrive")) param->setValueNotifyingHost (param->convertTo0to1 (9.602f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.2108f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0738f));
        break;
        default: break;
        }


    }


juce::SmoothedValue<float> cutoffSmooth;
juce::SmoothedValue<float> resonanceSmooth;
juce::SmoothedValue<float> predriveSmooth;
juce::SmoothedValue<float> gainSmooth;
juce::SmoothedValue<float> mixSmoothed;
float ic1eq[2] = {0.0f, 0.0f};
float ic2eq[2] = {0.0f, 0.0f};
float srF = 44100.0f;
inline float tubeSat(float x, float drive) {
  float d = juce::jlimit(0.01f, 20.0f, drive);
  float norm = tanhf(d);
  if (norm < 1e-6f) norm = 1e-6f;
  return tanhf(d * x) / norm;
}
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlowFilterProcessor)
};
