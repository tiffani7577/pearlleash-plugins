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


class GlitchKissProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    GlitchKissProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, juce::Identifier ("PARAMS"), createLayout())
    {
        // Portable FTZ (x86 MXCSR / arm64 FPCR). processBlock also uses juce::ScopedNoDenormals.
        pfEnableFlushToZero();

        licensed = GlitchKissLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("GlitchKiss: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("gatePattern", 0.0f, 1);
        versionManager.registerParameter ("mix", 0.5f, 1);
        versionManager.registerParameter ("gain", 0.0f, 1);


    }

    ~GlitchKissProcessor() override
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
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "gatePattern", 1 }, "Gate Pattern", juce::StringArray { "1/4 ON, 1/4 OFF", "1/8 ON, 1/8 OFF", "1/16 ON, 3/16 OFF", "1/4 ON, 1/8 OFF, 1/8 ON", "1/8 ON, 1/4 OFF, 1/8 ON", "Dotted 8th Feel" }, 0));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = 1347570764u; // seed lock: reproducible renders




        sm_gatePattern.reset (apvts.getRawParameterValue ("gatePattern")->load(), 10.0f, sampleRate);
        sm_mix.reset (apvts.getRawParameterValue ("mix")->load(), 5.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        samplesPerBeat = (60.0f / getBpm()) * (float) sampleRate;
currentBeatPosition = getPlayHeadBeatPosition();

mixSmoothed.reset(sampleRate, 0.020f);
mixSmoothed.setCurrentAndTargetValue(apvts.getRawParameterValue("mix")->load()); // Initialize with default mix value

// Define patterns. Each 'true' represents an 'on' state, 'false' an 'off' state.
// Each pattern is defined over 8 16th notes, which equals 2 beats.
// The pattern index (0-7) corresponds to a 16th note slot within this 2-beat cycle.

// Pattern 0: 1/4 note ON, 1/4 note OFF (repeated over 2 beats)
// This means 2 16ths ON, 2 16ths OFF. Over 8 16ths: [T T F F T T F F]
patterns[0] = {true, true, false, false, true, true, false, false};

// Pattern 1: 1/8 note ON, 1/8 note OFF (repeated over 2 beats)
// This means 1 16th ON, 1 16th OFF. Over 8 16ths: [T F T F T F T F]
patterns[1] = {true, false, true, false, true, false, true, false};

// Pattern 2: 1/16 note ON, 1/16 note OFF (repeated over 2 beats)
// This means 1 16th ON, 1 16th OFF. This is the same as pattern 1, but let's make it distinct.
// Let's make this a more complex 16th pattern: [T F F F T F F F] (1/16th on, 3/16th off)
patterns[2] = {true, false, false, false, true, false, false, false};

// Pattern 3: 1/4 note ON, 1/8 note OFF, 1/8 note ON (over 2 beats)
// Over 8 16ths: [T T F T T T F T]
patterns[3] = {true, true, false, true, true, true, false, true};

// Pattern 4: 1/8 note ON, 1/4 note OFF, 1/8 note ON (over 2 beats)
// Over 8 16ths: [T F F T T F F T]
patterns[4] = {true, false, false, true, true, false, false, true};

// Pattern 5: 1/16 note ON, 1/8 note OFF, 1/16 note ON (over 2 beats)
// Over 8 16ths: [T F F F T F F F] (This is the same as pattern 2, let's make it distinct)
// Let's make this a dotted 8th feel: [T T T F T T T F]
patterns[5] = {true, true, true, false, true, true, true, false};

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
        // ---- custom block: GlitchKiss (AI-generated) ----
float hostBpm = getBpm();
float hostBeatPosition = getPlayHeadBeatPosition();

// Update samplesPerBeat if BPM changes or if it's currently 0 (e.g., first run before host starts)
if (hostBpm > 0.0f) // Ensure BPM is valid to avoid division by zero
{
    samplesPerBeat = (60.0f / hostBpm) * (float)getSampleRate();
}

// If host transport is stopped, hostBeatPosition might be invalid or static.
// We need to ensure currentBeatPosition tracks the host's actual playhead.
// If the host is playing, we re-sync currentBeatPosition to hostBeatPosition at the start of the block.
// If the host is stopped, currentBeatPosition should not advance.

// Check if transport is playing. If not, currentBeatPosition should not advance.
// Assuming getPlayHeadIsPlaying() is available or infer from hostBeatPosition changes.
// For simplicity, we'll re-sync at the start of each block if hostBeatPosition changes significantly
// or if it's the first block (currentBeatPosition == 0.0f and hostBeatPosition != 0.0f).

// A more robust way to handle host sync: only update currentBeatPosition from hostBeatPosition
// if the host is playing and the difference is significant (e.g., after a jump or stop/start).
// For this simple block, we'll re-sync at the beginning of each block to the host's playhead.
// This ensures that if the host jumps, our internal beat position also jumps.
currentBeatPosition = hostBeatPosition;

// Get parameter values
int patternIndex = (int)apvts.getRawParameterValue("gatePattern")->load();
patternIndex = juce::jlimit(0, (int)patterns.size() - 1, patternIndex);

float mixParam = apvts.getRawParameterValue("mix")->load();
mixParam = juce::jlimit(0.0f, 1.0f, mixParam);
mixSmoothed.setTargetValue(mixParam);

for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
{
    auto* d = block.getChannelPointer((int)ch);
    for (size_t i = 0; i < block.getNumSamples(); ++i)
    {
        // TempoSyncEngine
        // The pattern is defined over 8 16th notes (2 beats).
        // We need to find which of the 8 slots we are currently in.
        // (currentBeatPosition % 2.0f) gives the beat position within a 2-beat cycle (0.0 to <2.0).
        // Multiply by 4.0f to get the 16th note index within that 2-beat cycle (0-7).
        float beatInCycle = fmodf(currentBeatPosition, 2.0f); 
        int patternSlot = (int)(beatInCycle * 4.0f); 
        patternSlot = juce::jlimit(0, (int)patterns[patternIndex].size() - 1, patternSlot);

        // VolumeGate
        bool gateState = patterns[patternIndex][patternSlot];
        float wetSignal = d[i] * (gateState ? 1.0f : 0.0f);

        // MixStage
        float currentMix = mixSmoothed.getNextValue();
        d[i] = d[i] * (1.0f - currentMix) + wetSignal * currentMix;

        // Advance internal beat position for the next sample
        // This ensures continuous tracking within the current processing block.
        if (samplesPerBeat > 0.0f) // Avoid division by zero if BPM is 0
        {
            currentBeatPosition += 1.0f / samplesPerBeat;
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
        sm_gatePattern.setTarget (apvts.getRawParameterValue ("gatePattern")->load());
        sm_gatePattern.update (getSampleRate());
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
    const juce::String getName() const override { return "GlitchKiss"; }
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
    WoManusSmoothedParameter<float> sm_gatePattern;
    WoManusSmoothedParameter<float> sm_mix;
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "gatePattern") == 0) return sm_gatePattern.getNextValue();
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
        if (auto* param = apvts.getParameter ("gatePattern")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("gatePattern")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5976f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-4.2717f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("gatePattern")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5229f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-4.0647f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("gatePattern")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4407f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-1.6397f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("gatePattern")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5525f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (3.7856f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("gatePattern")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4777f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.0029f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("gatePattern")) param->setValueNotifyingHost (param->convertTo0to1 (3.1171f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.6928f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-6.1094f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("gatePattern")) param->setValueNotifyingHost (param->convertTo0to1 (1.5571f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.3842f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-10.8403f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("gatePattern")) param->setValueNotifyingHost (param->convertTo0to1 (2.8655f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.1142f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (2.8498f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("gatePattern")) param->setValueNotifyingHost (param->convertTo0to1 (3.2168f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4671f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (5.1322f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("gatePattern")) param->setValueNotifyingHost (param->convertTo0to1 (4.1439f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.6716f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (3.0172f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("gatePattern")) param->setValueNotifyingHost (param->convertTo0to1 (0.3678f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9309f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-22.1249f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("gatePattern")) param->setValueNotifyingHost (param->convertTo0to1 (4.8247f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.063f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (22.244f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("gatePattern")) param->setValueNotifyingHost (param->convertTo0to1 (0.3469f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9819f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (23.8372f));
        break;
        default: break;
        }


    }


float currentBeatPosition = 0.0f;
float samplesPerBeat = 0.0f;
std::array<std::vector<bool>, 6> patterns;
juce::SmoothedValue<float> mixSmoothed;

    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlitchKissProcessor)
};
