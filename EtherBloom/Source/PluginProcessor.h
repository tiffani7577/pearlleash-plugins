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


class EtherBloomProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    EtherBloomProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, juce::Identifier ("PARAMS"), createLayout())
    {
        // Portable FTZ (x86 MXCSR / arm64 FPCR). processBlock also uses juce::ScopedNoDenormals.
        pfEnableFlushToZero();

        licensed = EtherBloomLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("EtherBloom: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("roomSize", 0.5f, 1);
        versionManager.registerParameter ("decay", 1.5f, 1);
        versionManager.registerParameter ("shimmer", 0.3f, 1);
        versionManager.registerParameter ("damping", 0.5f, 1);
        versionManager.registerParameter ("mix", 0.5f, 1);
        versionManager.registerParameter ("gain", 0.0f, 1);


    }

    ~EtherBloomProcessor() override
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
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "roomSize", 1 }, "Size", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "decay", 1 }, "Decay", juce::NormalisableRange<float> (0.1f, 4.0f), 1.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "shimmer", 1 }, "Shimmer", juce::NormalisableRange<float> (0.0f, 1.0f), 0.3f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "damping", 1 }, "Damping", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
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
        srF = (float) sampleRate;
for (size_t k = 0; k < (size_t) kNumLines; ++k){dlL[k].fill(0.0f);dlR[k].fill(0.0f);dlWrite[k]=0;}
grainBufL.fill(0.0f);grainBufR.fill(0.0f);
grainWrite=0;grainReadA=0.0f;grainReadB=(float)(kGrainLen/2);
for (size_t k = 0; k < (size_t) kNumLines; ++k){dampStateL[k]=0.0f;dampStateR[k]=0.0f;}
shimFbL=0.0f;shimFbR=0.0f;
rmsIn=0.0f;rmsOut=0.0f;agcGain=1.0f;
roomSmoothed.reset(srF,0.020f);roomSmoothed.setCurrentAndTargetValue(0.5f);
decaySmoothed.reset(srF,0.020f);decaySmoothed.setCurrentAndTargetValue(0.5f);
shimmerSmoothed.reset(srF,0.020f);shimmerSmoothed.setCurrentAndTargetValue(0.3f);
dampingSmoothed.reset(srF,0.020f);dampingSmoothed.setCurrentAndTargetValue(0.5f);
mixSmoothed.reset(srF,0.020f);mixSmoothed.setCurrentAndTargetValue(0.5f);
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
        // ---- custom block: EtherBloom (AI-generated) ----
const float bpm = juce::jmax(1.0f, getBpm());
(void)bpm;
float rawRoom = juce::jlimit(0.0f,1.0f,apvts.getRawParameterValue("roomSize")->load());
float rawDecay = juce::jlimit(0.1f,4.0f,apvts.getRawParameterValue("decay")->load());
float rawShim = juce::jlimit(0.0f,1.0f,apvts.getRawParameterValue("shimmer")->load());
float rawDamp = juce::jlimit(0.0f,1.0f,apvts.getRawParameterValue("damping")->load());
float rawMix = juce::jlimit(0.0f,1.0f,apvts.getRawParameterValue("mix")->load());
roomSmoothed.setTargetValue(rawRoom);
decaySmoothed.setTargetValue(rawDecay);
shimmerSmoothed.setTargetValue(rawShim);
dampingSmoothed.setTargetValue(rawDamp);
mixSmoothed.setTargetValue(rawMix);
const float hadamard[4][4] = {{0.5f,0.5f,0.5f,0.5f},{0.5f,-0.5f,0.5f,-0.5f},{0.5f,0.5f,-0.5f,-0.5f},{0.5f,-0.5f,-0.5f,0.5f}};
for(size_t i=0;i<block.getNumSamples();++i){
  float room = roomSmoothed.getNextValue();
  float decay = decaySmoothed.getNextValue();
  float shim = shimmerSmoothed.getNextValue();
  float damp = dampingSmoothed.getNextValue();
  float sizeScale = 0.3f + room * 1.7f;
  float fc = 800.0f + (1.0f - damp) * 7200.0f;
  float dampC = 1.0f - std::exp(-2.0f * 3.14159265f * fc / srF);
  float gShim = shim * 0.97f;
  float inL = (block.getNumChannels()>0)?block.getChannelPointer(0)[i]:0.0f;
  float inR = (block.getNumChannels()>1)?block.getChannelPointer(1)[i]:inL;
  float dryL = inL;
  float dryR = inR;
  float fdnInL = inL + shimFbL;
  float fdnInR = inR + shimFbR;
  float tapL[4], tapR[4];
  for (size_t k = 0; k < (size_t) kNumLines; ++k){
    int len = juce::jlimit(64,(int)(kMaxDL-1),(int)(kBaseLens[k]*sizeScale));
    dlLen[k]=len;
    float g = std::exp(-6.91f*(float)len/(decay*srF));
    g = juce::jlimit(0.0f,0.9999f,g);
    dlGain[k]=g;
    int rp = (dlWrite[k]-len+kMaxDL)%kMaxDL;
    tapL[k]=dlL[k][rp];
    tapR[k]=dlR[k][rp];
  }
  float mixL[4], mixR[4];
  for (size_t k = 0; k < (size_t) kNumLines; ++k){
    mixL[k]=0.0f;mixR[k]=0.0f;
    for (size_t j = 0; j < (size_t) kNumLines; ++j){mixL[k]+=hadamard[k][j]*tapL[j];mixR[k]+=hadamard[k][j]*tapR[j];}
  }
  float wetL=0.0f,wetR=0.0f;
  for (size_t k = 0; k < (size_t) kNumLines; ++k){
    float fbl = mixL[k]*dlGain[k];
    float fbr = mixR[k]*dlGain[k];
    dampStateL[k] += dampC*(fbl - dampStateL[k]);
    dampStateR[k] += dampC*(fbr - dampStateR[k]);
    float wl = fdnInL*0.25f + dampStateL[k];
    float wr = fdnInR*0.25f + dampStateR[k];
    wl = juce::jlimit(-4.0f,4.0f,wl);
    wr = juce::jlimit(-4.0f,4.0f,wr);
    dlL[k][dlWrite[k]]=wl;
    dlR[k][dlWrite[k]]=wr;
    dlWrite[k]=(dlWrite[k]+1)%kMaxDL;
    wetL+=tapL[k];
    wetR+=tapR[k];
  }
  wetL*=0.25f;
  wetR*=0.25f;
  grainBufL[grainWrite]=wetL;
  grainBufR[grainWrite]=wetR;
  grainReadA=std::fmod(grainReadA+2.0f,(float)kGrainLen);
  grainReadB=std::fmod(grainReadB+2.0f,(float)kGrainLen);
  int iaA=(int)grainReadA;
  int iaB=(int)grainReadB;
  float fracA=grainReadA-(float)iaA;
  float fracB=grainReadB-(float)iaB;
  float posA=std::fmod(grainReadA,(float)kGrainLen);
  float posB=std::fmod(grainReadB,(float)kGrainLen);
  float hannA=0.5f*(1.0f-std::cos(2.0f*3.14159265f*posA/(float)kGrainLen));
  float hannB=0.5f*(1.0f-std::cos(2.0f*3.14159265f*posB/(float)kGrainLen));
  int idxA0=(grainWrite-(int)posA+2048)%kGrainLen;
  int idxA1=(idxA0+1)%kGrainLen;
  int idxB0=(grainWrite-(int)posB+2048)%kGrainLen;
  int idxB1=(idxB0+1)%kGrainLen;
  float gL = (grainBufL[idxA0]*(1.0f-fracA)+grainBufL[idxA1]*fracA)*hannA
           + (grainBufL[idxB0]*(1.0f-fracB)+grainBufL[idxB1]*fracB)*hannB;
  float gR = (grainBufR[idxA0]*(1.0f-fracA)+grainBufR[idxA1]*fracA)*hannA
           + (grainBufR[idxB0]*(1.0f-fracB)+grainBufR[idxB1]*fracB)*hannB;
  shimFbL = gL * gShim;
  shimFbR = gR * gShim;
  shimFbL = juce::jlimit(-2.0f,2.0f,shimFbL);
  shimFbR = juce::jlimit(-2.0f,2.0f,shimFbR);
  rmsIn = 0.9999f*rmsIn + 0.0001f*(inL*inL+inR*inR)*0.5f;
  rmsOut = 0.9999f*rmsOut + 0.0001f*(wetL*wetL+wetR*wetR)*0.5f;
  float rmsInSqrt = std::sqrt(juce::jmax(1e-12f,rmsIn));
  float rmsOutSqrt = std::sqrt(juce::jmax(1e-12f,rmsOut));
  float targetAgc = rmsInSqrt/(rmsOutSqrt+1e-9f);
  targetAgc = juce::jlimit(0.1f,4.0f,targetAgc);
  agcGain = agcGain*0.9999f + targetAgc*0.0001f;
  float wetLfin = wetL * agcGain;
  float wetRfin = wetR * agcGain;
  wetRfin = wetRfin*0.866f + wetLfin*0.134f;
  wetLfin = wetLfin*0.866f + wetRfin*0.134f;
  wetLfin = juce::jlimit(-1.5f,1.5f,wetLfin);
  wetRfin = juce::jlimit(-1.5f,1.5f,wetRfin);
  grainWrite=(grainWrite+1)%kGrainLen;
  float mixVal = mixSmoothed.getNextValue();
  float dry = dryL;
  float wet = wetLfin;
  float outL = dry*(1.0f-mixVal)+wet*mixVal;
  float dry2 = dryR;
  float wet2 = wetRfin;
  float outR = dry2*(1.0f-mixVal)+wet2*mixVal;
  if(block.getNumChannels()>0)block.getChannelPointer(0)[i]=outL;
  if(block.getNumChannels()>1)block.getChannelPointer(1)[i]=outR;
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
    const juce::String getName() const override { return "EtherBloom"; }
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
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5257f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (1.583f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.4412f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5861f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-4.3566f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5747f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (1.3084f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.4353f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5325f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-0.5191f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5706f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (1.5959f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.3336f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.5037f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4599f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.8294f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5237f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (1.3742f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.3056f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.5721f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4038f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0003f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5329f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (1.27f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.3615f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.559f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5273f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-0.0086f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.9688f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.8757f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.5203f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.3245f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.168f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (11.8151f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.6862f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (2.7921f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.6569f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.323f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.1004f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-16.9085f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.6839f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (2.799f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.2922f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.099f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5123f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-10.7022f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.4187f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (2.9491f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.6248f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.534f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7831f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (9.2175f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5949f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (2.3991f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.7958f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.9002f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4537f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (6.616f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.0074f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.3112f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.0616f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.0195f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0244f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-23.3054f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.9799f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.3762f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.0452f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.9759f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0579f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (23.9476f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.9726f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (3.7491f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.933f));
        if (auto* param = apvts.getParameter ("damping")) param->setValueNotifyingHost (param->convertTo0to1 (0.0719f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9935f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (21.2604f));
        break;
        default: break;
        }


    }


static constexpr int kMaxDL = 131072;
static constexpr int kNumLines = 4;
static constexpr int kGrainLen = 1024;
std::array<std::array<float,131072>,4> dlL{};
std::array<std::array<float,131072>,4> dlR{};
std::array<int,4> dlWrite{};
std::array<int,4> dlLen{};
static constexpr std::array<float,4> kBaseLens = {1283.0f,1699.0f,2503.0f,3929.0f};
std::array<float,4> dlGain{};
std::array<float,4> dampStateL{};
std::array<float,4> dampStateR{};
float shimFbL = 0.0f, shimFbR = 0.0f;
std::array<float,2048> grainBufL{};
std::array<float,2048> grainBufR{};
int grainWrite = 0;
float grainReadA = 0.0f, grainReadB = 0.0f;
float srF = 44100.0f;
juce::SmoothedValue<float> roomSmoothed;
juce::SmoothedValue<float> decaySmoothed;
juce::SmoothedValue<float> shimmerSmoothed;
juce::SmoothedValue<float> dampingSmoothed;
juce::SmoothedValue<float> mixSmoothed;
float rmsIn = 0.0f, rmsOut = 0.0f, agcGain = 1.0f;
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EtherBloomProcessor)
};
