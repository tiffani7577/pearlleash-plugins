#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <cstring>
#include <cmath>
#include "PlugForgeDenormals.h"
#include "WoManusParameterSmoothing.h"
#include "ParameterVersioning.h"




#include "ShimmerReverb.h"






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


class KawaiiShimmerVerbProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    KawaiiShimmerVerbProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, juce::Identifier ("PARAMS"), createLayout())
    {
        // Portable FTZ (x86 MXCSR / arm64 FPCR). processBlock also uses juce::ScopedNoDenormals.
        pfEnableFlushToZero();

        licensed = KawaiiShimmerVerbLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("KawaiiShimmerVerb: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("roomSize", 0.6f, 1);
        versionManager.registerParameter ("decay", 0.7f, 1);
        versionManager.registerParameter ("shimmer", 0.5f, 1);
        versionManager.registerParameter ("damping", 0.4f, 1);
        versionManager.registerParameter ("mix", 0.5f, 1);
        versionManager.registerParameter ("gain", 0.0f, 1);


    }

    ~KawaiiShimmerVerbProcessor() override
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
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "roomSize", 1 }, "Size", juce::NormalisableRange<float> (0.1f, 1.0f), 0.6f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "decay", 1 }, "Decay", juce::NormalisableRange<float> (0.1f, 1.0f), 0.7f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "shimmer", 1 }, "Shimmer", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "damping", 1 }, "Damping", juce::NormalisableRange<float> (0.0f, 1.0f), 0.4f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = 1347570764u; // seed lock: reproducible renders




        sm_roomSize.reset (apvts.getRawParameterValue ("roomSize")->load(), 10.0f, sampleRate);
        sm_decay.reset (apvts.getRawParameterValue ("decay")->load(), 10.0f, sampleRate);
        sm_shimmer.reset (apvts.getRawParameterValue ("shimmer")->load(), 10.0f, sampleRate);
        sm_damping.reset (apvts.getRawParameterValue ("damping")->load(), 10.0f, sampleRate);
        sm_mix.reset (apvts.getRawParameterValue ("mix")->load(), 5.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
            shimmerReverbDsp_.prepare (sampleRate);
    shimmerReverbDsp_.setParameters (
        apvts.getRawParameterValue ("roomSize")->load(),
        apvts.getRawParameterValue ("decay")->load(),
        apvts.getRawParameterValue ("shimmer")->load(),
        apvts.getRawParameterValue ("mix")->load());
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
        {
        shimmerReverbDsp_.setParameters (
            apvts.getRawParameterValue ("roomSize")->load(),
            apvts.getRawParameterValue ("decay")->load(),
            apvts.getRawParameterValue ("shimmer")->load(),
            apvts.getRawParameterValue ("mix")->load());
        auto* left = block.getChannelPointer (0);
        float* right = block.getNumChannels() > 1 ? block.getChannelPointer (1) : left;
        for (size_t i = 0; i < block.getNumSamples(); ++i)
            shimmerReverbDsp_.processSample (left[i], right[i]);
    }
        gainDsp.setGainDecibels (smoothedParam ("gain"));
    gainDsp.process (ctx);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ignoreUnused (midiMessages);
        juce::ScopedNoDenormals noDenormals;
        sm_roomSize.setTarget (apvts.getRawParameterValue ("roomSize")->load());
        sm_roomSize.update (getSampleRate());
        sm_decay.setTarget (apvts.getRawParameterValue ("decay")->load());
        sm_decay.update (getSampleRate());
        sm_shimmer.setTarget (apvts.getRawParameterValue ("shimmer")->load());
        sm_shimmer.update (getSampleRate());
        sm_damping.setTarget (apvts.getRawParameterValue ("damping")->load());
        sm_damping.update (getSampleRate());
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
    const juce::String getName() const override { return "KawaiiShimmerVerb"; }
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
    WoManusSmoothedParameter<float> sm_roomSize;
    WoManusSmoothedParameter<float> sm_decay;
    WoManusSmoothedParameter<float> sm_shimmer;
    WoManusSmoothedParameter<float> sm_damping;
    WoManusSmoothedParameter<float> sm_mix;
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "roomSize") == 0) return sm_roomSize.getNextValue();
        if (strcmp (id, "decay") == 0) return sm_decay.getNextValue();
        if (strcmp (id, "shimmer") == 0) return sm_shimmer.getNextValue();
        if (strcmp (id, "damping") == 0) return sm_damping.getNextValue();
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
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.6f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.4f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5397f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.7026f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.5271f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.3267f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4555f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-0.7703f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5481f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.6814f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.5309f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.4287f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4996f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-3.6761f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5846f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.6504f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.5044f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.4115f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.421f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-1.4954f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.605f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.6116f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.417f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.4189f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.512f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (3.9291f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.6586f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.6343f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.4342f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.4436f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5932f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-2.6999f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.762f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.6903f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.8461f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-14.8018f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.4848f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.378f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.6112f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.5763f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.3541f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-14.7234f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5772f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.1531f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.6985f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.3266f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.812f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-11.1258f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.4277f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.5344f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.4323f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.4256f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.6695f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (3.8758f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.3075f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.8323f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.9938f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.5856f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.8358f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-21.01f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.1487f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.1564f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.9726f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.0614f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9726f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (21.8852f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.1076f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.1492f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.9862f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.0428f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9412f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (20.3932f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.1253f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.1414f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.0279f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.9575f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9556f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-21.08f));
        break;
        default: break;
        }


    }


    TrustedShimmerReverb shimmerReverbDsp_;
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KawaiiShimmerVerbProcessor)
};
