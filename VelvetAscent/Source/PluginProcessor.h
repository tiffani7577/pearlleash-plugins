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


class VelvetAscentProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    VelvetAscentProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, juce::Identifier ("PARAMS"), createLayout())
    {
        // Portable FTZ (x86 MXCSR / arm64 FPCR). processBlock also uses juce::ScopedNoDenormals.
        pfEnableFlushToZero();

        licensed = VelvetAscentLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("VelvetAscent: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("roomSize", 0.5f, 1);
        versionManager.registerParameter ("decay", 2.5f, 1);
        versionManager.registerParameter ("damping", 0.5f, 1);
        versionManager.registerParameter ("shimmer", 0.5f, 1);
        versionManager.registerParameter ("mix", 0.5f, 1);
        versionManager.registerParameter ("gain", 0.0f, 1);


    }

    ~VelvetAscentProcessor() override
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
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "roomSize", 1 }, "Room Size", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "decay", 1 }, "Decay", juce::NormalisableRange<float> (0.5f, 10.0f), 2.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "damping", 1 }, "Damping", juce::NormalisableRange<float> (0.0f, 0.95f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "shimmer", 1 }, "Shimmer", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = 1347570764u; // seed lock: reproducible renders




        sm_roomSize.reset (apvts.getRawParameterValue ("roomSize")->load(), 10.0f, sampleRate);
        sm_decay.reset (apvts.getRawParameterValue ("decay")->load(), 10.0f, sampleRate);
        sm_damping.reset (apvts.getRawParameterValue ("damping")->load(), 10.0f, sampleRate);
        sm_shimmer.reset (apvts.getRawParameterValue ("shimmer")->load(), 10.0f, sampleRate);
        sm_mix.reset (apvts.getRawParameterValue ("mix")->load(), 5.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        srF = (float) sampleRate;
for(auto& b : fdnBuf) b.fill(0.0f);
fdnWrite.fill(0);
fdnLen.fill(0);
dampState.fill(0.0f);
fdnOut.fill(0.0f);
grainBuf.fill(0.0f);
grainWrite = 0;
grain0Phase = 0.0f;
grain1Phase = 0.5f;
agcGain = 1.0f;
rmsIn = 0.0f;
rmsOut = 0.0f;
phaseDecorL = 0.0f;
phaseDecorR = 0.0f;
roomSmoothed.reset(srF, 0.020f); roomSmoothed.setCurrentAndTargetValue(0.5f);
mixSmoothed.reset(srF, 0.020f); mixSmoothed.setCurrentAndTargetValue(0.5f);
shimmerSmoothed.reset(srF, 0.020f); shimmerSmoothed.setCurrentAndTargetValue(0.5f);
decaySmoothed.reset(srF, 0.020f); decaySmoothed.setCurrentAndTargetValue(2.5f);
dampSmoothed.reset(srF, 0.020f); dampSmoothed.setCurrentAndTargetValue(0.5f);
agcSmoothed.reset(srF, 0.200f); agcSmoothed.setCurrentAndTargetValue(1.0f);
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
        // ---- custom block: VelvetAscent (AI-generated) ----
const float bpm = juce::jmax(1.0f, getBpm());
(void)bpm;
float rawRoom = juce::jlimit(0.0f,1.0f, apvts.getRawParameterValue("roomSize")->load());
float rawMix = juce::jlimit(0.0f,1.0f, apvts.getRawParameterValue("mix")->load());
float rawShimmer = juce::jlimit(0.0f,1.0f, apvts.getRawParameterValue("shimmer")->load());
float rawDecay = juce::jlimit(0.5f,10.0f, apvts.getRawParameterValue("decay")->load());
float rawDamp = juce::jlimit(0.0f,0.95f, apvts.getRawParameterValue("damping")->load());
roomSmoothed.setTargetValue(rawRoom);
mixSmoothed.setTargetValue(rawMix);
shimmerSmoothed.setTargetValue(rawShimmer);
decaySmoothed.setTargetValue(rawDecay);
dampSmoothed.setTargetValue(rawDamp);
const int primes[4] = {29,37,43,53};
const float H[4][4] = {{0.5f,0.5f,0.5f,0.5f},{0.5f,-0.5f,0.5f,-0.5f},{0.5f,0.5f,-0.5f,-0.5f},{0.5f,-0.5f,-0.5f,0.5f}};
const int grainSize = (int)(0.08f * srF);
const int hopSize = grainSize / 2;
const float pitchRatio = 2.0f;
auto* dL = block.getChannelPointer(0);
auto* dR = block.getNumChannels() > 1 ? block.getChannelPointer(1) : block.getChannelPointer(0);
for(size_t i = 0; i < block.getNumSamples(); ++i) {
  float roomVal = roomSmoothed.getNextValue();
  float dampVal = dampSmoothed.getNextValue();
  float decayVal = decaySmoothed.getNextValue();
  float shimmerVal = shimmerSmoothed.getNextValue();
  float roomMul = 0.3f * std::exp(std::log(10.0f) * roomVal);
  for (size_t n = 0; n < (size_t) 4; ++n) {
    int len = (int)(roomMul * primes[n] * srF / 1000.0f);
    len = juce::jlimit(64, kMaxDel-1, len);
    fdnLen[n] = len;
  }
  float inL = dL[i];
  float inR = dR[i];
  float monoIn = (inL + inR) * 0.5f;
  phaseDecorL += (monoIn - phaseDecorL) * 0.9f;
  phaseDecorR += (monoIn - phaseDecorR) * 0.85f;
  float decL = inL + (inL - phaseDecorL) * 0.3f;
  float decR = inR + (inR - phaseDecorR) * 0.3f;
  float fdnIn = (decL + decR) * 0.5f;
  grainBuf[grainWrite & 8191] = fdnOut[0] + fdnOut[1] + fdnOut[2] + fdnOut[3];
  grain0Phase += pitchRatio / (float)grainSize;
  grain1Phase += pitchRatio / (float)grainSize;
  if(grain0Phase >= 1.0f) grain0Phase -= 1.0f;
  if(grain1Phase >= 1.0f) grain1Phase -= 1.0f;
  int g0idx = (grainWrite - (int)(grain0Phase * grainSize) + 8192) & 8191;
  int g1idx = (grainWrite - hopSize - (int)(grain1Phase * grainSize) + 8192) & 8191;
  float win0 = 0.5f - 0.5f * std::cos(6.28318f * grain0Phase);
  float win1 = 0.5f - 0.5f * std::cos(6.28318f * grain1Phase);
  float pitchOut = grainBuf[g0idx] * win0 + grainBuf[g1idx] * win1;
  grainWrite = (grainWrite + 1) & 8191;
  fdnIn += shimmerVal * pitchOut * 0.4f;
  float decayGain = std::pow(0.001f, 1.0f / (decayVal * srF));
  std::array<float,4> nodeIn;
  for (size_t n = 0; n < (size_t) 4; ++n) {
    int rp = (fdnWrite[n] - fdnLen[n] + kMaxDel) % kMaxDel;
    fdnOut[n] = fdnBuf[n][rp];
  }
  for (size_t n = 0; n < (size_t) 4; ++n) {
    float mixed = 0.0f;
    for (size_t m = 0; m < (size_t) 4; ++m) mixed += H[n][m] * fdnOut[m];
    float g = std::pow(decayGain, (float)fdnLen[n]);
    dampState[n] = (1.0f - dampVal) * mixed + dampVal * dampState[n];
    nodeIn[n] = fdnIn + dampState[n] * g;
  }
  for (size_t n = 0; n < (size_t) 4; ++n) {
    fdnBuf[n][fdnWrite[n]] = nodeIn[n];
    fdnWrite[n] = (fdnWrite[n] + 1) % kMaxDel;
  }
  float wetMono = (fdnOut[0] + fdnOut[1] + fdnOut[2] + fdnOut[3]) * 0.5f;
  float wetL = (fdnOut[0] + fdnOut[2]) * 0.7071f;
  float wetR = (fdnOut[1] + fdnOut[3]) * 0.7071f;
  rmsIn = 0.9999f * rmsIn + 0.0001f * monoIn * monoIn;
  rmsOut = 0.9999f * rmsOut + 0.0001f * wetMono * wetMono;
  float targetAgc = juce::jlimit(0.25f, 4.0f, std::sqrt(rmsIn + 1e-12f) / (std::sqrt(rmsOut + 1e-12f) + 1e-6f));
  agcSmoothed.setTargetValue(targetAgc);
  agcGain = agcSmoothed.getNextValue();
  wetL *= agcGain;
  wetR *= agcGain;
  float mixVal = mixSmoothed.getNextValue();
  float dryL = dL[i];
  float dryR = dR[i];
  float processedL = wetL;
  float processedR = wetR;
  dL[i] = dryL * (1.0f - mixVal) + processedL * mixVal;
  dR[i] = dryR * (1.0f - mixVal) + processedR * mixVal;
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
        sm_damping.setTarget (apvts.getRawParameterValue ("damping")->load());
        sm_damping.update (getSampleRate());
        sm_shimmer.setTarget (apvts.getRawParameterValue ("shimmer")->load());
        sm_shimmer.update (getSampleRate());
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
    const juce::String getName() const override { return "VelvetAscent"; }
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
    WoManusSmoothedParameter<float> sm_damping;
    WoManusSmoothedParameter<float> sm_shimmer;
    WoManusSmoothedParameter<float> sm_mix;
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "roomSize") == 0) return sm_roomSize.getNextValue();
        if (strcmp (id, "decay") == 0) return sm_decay.getNextValue();
        if (strcmp (id, "damping") == 0) return sm_damping.getNextValue();
        if (strcmp (id, "shimmer") == 0) return sm_shimmer.getNextValue();
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
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (2.5f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.4118f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (3.35f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.4626f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.4506f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5493f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-1.8972f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5348f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (3.4453f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.5357f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.4941f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4045f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.7767f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5142f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (3.35f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.484f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.5479f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.424f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-3.3741f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5401f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (3.35f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.4959f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.4214f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4445f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.3369f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5761f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (3.35f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.5195f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.4775f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4958f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (2.1048f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5477f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (9.2534f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.3039f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.8134f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5575f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-13.9306f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.6345f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (5.9621f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.2785f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.5984f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.3041f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-11.8232f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.0758f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.9962f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.3141f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.7901f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.433f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.7998f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.4376f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (6.9484f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.3229f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.6582f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.498f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.7945f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.6211f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (7.4708f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.5214f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.7147f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5129f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (11.1667f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.0647f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.9259f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.9035f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.938f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9724f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-23.0648f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.9298f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.8028f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.0546f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.9646f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9425f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (21.5702f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.9328f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.884f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.8878f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.0733f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0457f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (21.659f));
        break;
        default: break;
        }


    }


static constexpr int kMaxDel = 262144;
static constexpr int kNumNodes = 4;
static constexpr int kGrainSamples = 4096;
std::array<std::array<float,262144>,4> fdnBuf{};
std::array<int,4> fdnWrite{};
std::array<int,4> fdnLen{};
std::array<float,4> dampState{};
std::array<float,4> fdnOut{};
std::array<float,8192> grainBuf{};
int grainWrite = 0;
int grain0Read = 0;
int grain1Read = 0;
int grain0Pos = 0;
float grain0Phase = 0.0f;
float grain1Phase = 0.5f;
float srF = 44100.0f;
float agcGain = 1.0f;
float rmsIn = 0.0f;
float rmsOut = 0.0f;
juce::SmoothedValue<float> roomSmoothed;
juce::SmoothedValue<float> mixSmoothed;
juce::SmoothedValue<float> shimmerSmoothed;
juce::SmoothedValue<float> decaySmoothed;
juce::SmoothedValue<float> dampSmoothed;
juce::SmoothedValue<float> agcSmoothed;
float phaseDecorL = 0.0f;
float phaseDecorR = 0.0f;
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VelvetAscentProcessor)
};
