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


class SakuraShimmerProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    SakuraShimmerProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, juce::Identifier ("PARAMS"), createLayout())
    {
        // Portable FTZ (x86 MXCSR / arm64 FPCR). processBlock also uses juce::ScopedNoDenormals.
        pfEnableFlushToZero();

        licensed = SakuraShimmerLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("SakuraShimmer: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("roomSize", 0.5f, 1);
        versionManager.registerParameter ("shimmer", 0.6f, 1);
        versionManager.registerParameter ("decay", 0.7f, 1);
        versionManager.registerParameter ("mix", 0.5f, 1);
        versionManager.registerParameter ("gain", 0.0f, 1);


    }

    ~SakuraShimmerProcessor() override
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
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "roomSize", 1 }, "Bloom Size", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "shimmer", 1 }, "Shimmer", juce::NormalisableRange<float> (0.0f, 1.0f), 0.6f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "decay", 1 }, "Decay", juce::NormalisableRange<float> (0.0f, 1.0f), 0.7f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = 1347570764u; // seed lock: reproducible renders




        sm_roomSize.reset (apvts.getRawParameterValue ("roomSize")->load(), 10.0f, sampleRate);
        sm_shimmer.reset (apvts.getRawParameterValue ("shimmer")->load(), 10.0f, sampleRate);
        sm_decay.reset (apvts.getRawParameterValue ("decay")->load(), 10.0f, sampleRate);
        sm_mix.reset (apvts.getRawParameterValue ("mix")->load(), 5.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        srF=(float) sampleRate;
for(auto&b:apBufL0)b=0;for(auto&b:apBufL1)b=0;for(auto&b:apBufL2)b=0;for(auto&b:apBufL3)b=0;
for(auto&b:apBufR0)b=0;for(auto&b:apBufR1)b=0;for(auto&b:apBufR2)b=0;for(auto&b:apBufR3)b=0;
for(auto&b:fdnL0)b=0;for(auto&b:fdnL1)b=0;for(auto&b:fdnL2)b=0;for(auto&b:fdnL3)b=0;
for(auto&b:fdnR0)b=0;for(auto&b:fdnR1)b=0;for(auto&b:fdnR2)b=0;for(auto&b:fdnR3)b=0;
for(auto&b:grainBufL)b=0;for(auto&b:grainBufR)b=0;
apWr={0,0,0,0};fdnWr={0,0,0,0};
grainWr=0;grainGrainPos0=0;grainGrainPos1=0;grainPhase0=0.0f;grainPhase1=0.5f;
fbL0=fbL1=fbL2=fbL3=0.0f;fbR0=fbR1=fbR2=fbR3=0.0f;
hsStateL=hsStateR=0.0f;silkStateL=silkStateR=0.0f;
roomSmoothed.reset(srF,0.020f);roomSmoothed.setCurrentAndTargetValue(0.5f);
shimmerSmoothed.reset(srF,0.020f);shimmerSmoothed.setCurrentAndTargetValue(0.6f);
decaySmoothed.reset(srF,0.020f);decaySmoothed.setCurrentAndTargetValue(0.7f);
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
        // ---- custom block: SakuraShimmer (AI-generated) ----
const float bpm = juce::jmax(1.0f, getBpm());
(void)bpm;
float rawRoom = juce::jlimit(0.0f,1.0f,apvts.getRawParameterValue("roomSize")->load());
float rawShimmer = juce::jlimit(0.0f,1.0f,apvts.getRawParameterValue("shimmer")->load());
float rawDecay = juce::jlimit(0.0f,1.0f,apvts.getRawParameterValue("decay")->load());
float rawMix = juce::jlimit(0.0f,1.0f,apvts.getRawParameterValue("mix")->load());
roomSmoothed.setTargetValue(rawRoom);
shimmerSmoothed.setTargetValue(rawShimmer);
decaySmoothed.setTargetValue(rawDecay);
mixSmoothed.setTargetValue(rawMix);
const float apG = 0.6f;
const int apBaseLens[4]={401,631,997,1511};
const int fdnBaseLens[4]={4799,6247,7919,9973};
const int grainSize = (int)(0.05f*srF);
const int grainSizeMask = kMaxGrain-1;
const float hsA = std::exp(-2.0f*3.14159265f*8000.0f/srF);
const float hsGain = 0.3f;
size_t nCh = block.getNumChannels();
size_t nSamp = block.getNumSamples();
float* dL = block.getChannelPointer(0);
float* dR = (nCh>1)?block.getChannelPointer(1):nullptr;
for(size_t i=0;i<nSamp;++i){
  float room = roomSmoothed.getNextValue();
  float shimmer = shimmerSmoothed.getNextValue();
  float decay = decaySmoothed.getNextValue();
  float mixVal = mixSmoothed.getNextValue();
  float fb = decay*0.95f;
  int apLen0=juce::jlimit(1,kMaxAP-1,(int)(apBaseLens[0]*(0.3f+0.7f*room)));
  int apLen1=juce::jlimit(1,kMaxAP-1,(int)(apBaseLens[1]*(0.3f+0.7f*room)));
  int apLen2=juce::jlimit(1,kMaxAP-1,(int)(apBaseLens[2]*(0.3f+0.7f*room)));
  int apLen3=juce::jlimit(1,kMaxAP-1,(int)(apBaseLens[3]*(0.3f+0.7f*room)));
  int fLen0=juce::jlimit(1,kMaxFDN-1,(int)(fdnBaseLens[0]*(0.3f+0.7f*room)));
  int fLen1=juce::jlimit(1,kMaxFDN-1,(int)(fdnBaseLens[1]*(0.3f+0.7f*room)));
  int fLen2=juce::jlimit(1,kMaxFDN-1,(int)(fdnBaseLens[2]*(0.3f+0.7f*room)));
  int fLen3=juce::jlimit(1,kMaxFDN-1,(int)(fdnBaseLens[3]*(0.3f+0.7f*room)));
  float inL=dL[i], inR=(dR?dR[i]:inL);
  float dryL=inL, dryR=inR;
  // Allpass diffuser L
  auto apProc=[&](std::array<float,8192>&buf,int&wr,int len,float x)->float{
    int rd=(wr-len+kMaxAP)%kMaxAP;
    float bv=buf[rd];
    float w=x-apG*bv;
    buf[wr]=w;
    wr=(wr+1)%kMaxAP;
    return bv+apG*w;
  };
  float aL=apProc(apBufL0,apWr[0],apLen0,inL);
  aL=apProc(apBufL1,apWr[1],apLen1,aL);
  aL=apProc(apBufL2,apWr[2],apLen2,aL);
  aL=apProc(apBufL3,apWr[3],apLen3,aL);
  float aR=apProc(apBufR0,apWr[0],apLen0,inR);
  aR=apProc(apBufR1,apWr[1],apLen1,aR);
  aR=apProc(apBufR2,apWr[2],apLen2,aR);
  aR=apProc(apBufR3,apWr[3],apLen3,aR);
  // Write to grain block
  grainBufL[grainWr&grainSizeMask]=aL;
  grainBufR[grainWr&grainSizeMask]=aR;
  // Granular pitch shift (2x, overlap-add, 2 grains)
  auto hannW=[](float ph)->float{return 0.5f-0.5f*std::cos(2.0f*3.14159265f*ph);};
  float pShL=0.0f,pShR=0.0f;
  // Grain 0
  {
    float ph=grainPhase0;
    int rp=(grainWr-(int)((1.0f-ph)*grainSize)+kMaxGrain)&grainSizeMask;
    float w=hannW(ph);
    pShL+=grainBufL[rp]*w;
    pShR+=grainBufR[rp]*w;
    grainPhase0+=2.0f/(float)grainSize;
    if(grainPhase0>=1.0f)grainPhase0-=1.0f;
  }
  // Grain 1 (offset by 0.5)
  {
    float ph=grainPhase1;
    int rp=(grainWr-(int)((1.0f-ph)*grainSize)+kMaxGrain)&grainSizeMask;
    float w=hannW(ph);
    pShL+=grainBufL[rp]*w;
    pShR+=grainBufR[rp]*w;
    grainPhase1+=2.0f/(float)grainSize;
    if(grainPhase1>=1.0f)grainPhase1-=1.0f;
  }
  grainWr=(grainWr+1)&grainSizeMask;
  // FDN inputs: diffused + shimmer pitch shifted feedback
  float fdnInL=aL*(1.0f-shimmer)+pShL*shimmer;
  float fdnInR=aR*(1.0f-shimmer)+pShR*shimmer;
  // FDN read
  auto fdnRead=[&](std::array<float,65536>&buf,int wr,int len)->float{
    int rd=(wr-len+kMaxFDN)%kMaxFDN;
    return buf[rd];
  };
  float f0L=fdnRead(fdnL0,fdnWr[0],fLen0);
  float f1L=fdnRead(fdnL1,fdnWr[1],fLen1);
  float f2L=fdnRead(fdnL2,fdnWr[2],fLen2);
  float f3L=fdnRead(fdnL3,fdnWr[3],fLen3);
  float f0R=fdnRead(fdnR0,fdnWr[0],fLen0);
  float f1R=fdnRead(fdnR1,fdnWr[1],fLen1);
  float f2R=fdnRead(fdnR2,fdnWr[2],fLen2);
  float f3R=fdnRead(fdnR3,fdnWr[3],fLen3);
  // Hadamard mix
  float h0L=f0L+f1L+f2L+f3L;
  float h1L=f0L-f1L+f2L-f3L;
  float h2L=f0L+f1L-f2L-f3L;
  float h3L=f0L-f1L-f2L+f3L;
  float h0R=f0R+f1R+f2R+f3R;
  float h1R=f0R-f1R+f2R-f3R;
  float h2R=f0R+f1R-f2R-f3R;
  float h3R=f0R-f1R-f2R+f3R;
  float sc=0.5f;
  // High shelf silk on feedback
  float silkInL=(h0L+h1L+h2L+h3L)*sc*0.25f;
  float silkInR=(h0R+h1R+h2R+h3R)*sc*0.25f;
  silkStateL=hsA*silkStateL+(1.0f-hsA)*silkInL;
  silkStateR=hsA*silkStateR+(1.0f-hsA)*silkInR;
  float silkL=silkInL+hsGain*(silkInL-silkStateL);
  float silkR=silkInR+hsGain*(silkInR-silkStateR);
  // FDN write
  fdnL0[fdnWr[0]]=(fdnInL+h0L*sc)*fb+silkL*0.1f;
  fdnL1[fdnWr[1]]=(fdnInL+h1L*sc)*fb+silkL*0.1f;
  fdnL2[fdnWr[2]]=(fdnInL+h2L*sc)*fb+silkL*0.1f;
  fdnL3[fdnWr[3]]=(fdnInL+h3L*sc)*fb+silkL*0.1f;
  fdnR0[fdnWr[0]]=(fdnInR+h0R*sc)*fb+silkR*0.1f;
  fdnR1[fdnWr[1]]=(fdnInR+h1R*sc)*fb+silkR*0.1f;
  fdnR2[fdnWr[2]]=(fdnInR+h2R*sc)*fb+silkR*0.1f;
  fdnR3[fdnWr[3]]=(fdnInR+h3R*sc)*fb+silkR*0.1f;
  fdnWr[0]=(fdnWr[0]+1)%kMaxFDN;
  fdnWr[1]=(fdnWr[1]+1)%kMaxFDN;
  fdnWr[2]=(fdnWr[2]+1)%kMaxFDN;
  fdnWr[3]=(fdnWr[3]+1)%kMaxFDN;
  // High shelf silk on output
  float wetL=(f0L+f1L+f2L+f3L)*0.25f;
  float wetR=(f0R+f1R+f2R+f3R)*0.25f;
  float silkOutStateL=silkStateL;
  float silkOutStateR=silkStateR;
  float wetSilkL=wetL+hsGain*(wetL-silkOutStateL);
  float wetSilkR=wetR+hsGain*(wetR-silkOutStateR);
  // Wet/dry
  float dry=dryL;
  float wet=wetSilkL;
  float outL=dry*(1.0f-mixVal)+wet*mixVal;
  dry=dryR;
  wet=wetSilkR;
  float outR=dry*(1.0f-mixVal)+wet*mixVal;
  dL[i]=juce::jlimit(-2.0f,2.0f,outL);
  if(dR)dR[i]=juce::jlimit(-2.0f,2.0f,outR);
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
        sm_shimmer.setTarget (apvts.getRawParameterValue ("shimmer")->load());
        sm_shimmer.update (getSampleRate());
        sm_decay.setTarget (apvts.getRawParameterValue ("decay")->load());
        sm_decay.update (getSampleRate());
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
    const juce::String getName() const override { return "SakuraShimmer"; }
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
    WoManusSmoothedParameter<float> sm_shimmer;
    WoManusSmoothedParameter<float> sm_decay;
    WoManusSmoothedParameter<float> sm_mix;
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "roomSize") == 0) return sm_roomSize.getNextValue();
        if (strcmp (id, "shimmer") == 0) return sm_shimmer.getNextValue();
        if (strcmp (id, "decay") == 0) return sm_decay.getNextValue();
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
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.6f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.4803f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.6407f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.6154f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4825f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (1.9376f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5486f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.6474f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4478f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-4.4994f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.4464f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.6582f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5116f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-3.6986f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5916f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.6387f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.6047f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4886f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-1.5862f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5331f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.6793f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4139f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-1.6834f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.2303f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.4939f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.7969f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (15.6618f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.6389f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.2718f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.3046f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5487f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-0.0044f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.3121f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.3612f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.3842f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.1233f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.5057f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.5135f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.5965f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.7076f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5829f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-4.1255f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.9446f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.7518f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.7622f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.795f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (13.7749f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.9477f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.9386f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.0071f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9409f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-20.2757f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.0172f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.0119f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.932f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-23.4155f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("roomSize")) param->setValueNotifyingHost (param->convertTo0to1 (0.9859f));
        if (auto* param = apvts.getParameter ("shimmer")) param->setValueNotifyingHost (param->convertTo0to1 (0.9377f));
        if (auto* param = apvts.getParameter ("decay")) param->setValueNotifyingHost (param->convertTo0to1 (0.0252f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9546f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (21.8505f));
        break;
        default: break;
        }


    }


static constexpr int kMaxAP = 8192;
static constexpr int kMaxFDN = 65536;
static constexpr int kMaxGrain = 8192;
std::array<float,8192> apBufL0{},apBufL1{},apBufL2{},apBufL3{};
std::array<float,8192> apBufR0{},apBufR1{},apBufR2{},apBufR3{};
std::array<int,4> apWr{};
std::array<float,65536> fdnL0{},fdnL1{},fdnL2{},fdnL3{};
std::array<float,65536> fdnR0{},fdnR1{},fdnR2{},fdnR3{};
std::array<int,4> fdnWr{};
std::array<int,4> fdnLen{};
std::array<float,8192> grainBufL{},grainBufR{};
int grainWr=0,grainGrainPos0=0,grainGrainPos1=0;
float grainPhase0=0.0f,grainPhase1=0.0f;
float hsStateL=0.0f,hsStateR=0.0f;
float fbL0=0.0f,fbL1=0.0f,fbL2=0.0f,fbL3=0.0f;
float fbR0=0.0f,fbR1=0.0f,fbR2=0.0f,fbR3=0.0f;
float srF=44100.0f;
juce::SmoothedValue<float> roomSmoothed,shimmerSmoothed,decaySmoothed,mixSmoothed;
float silkStateL=0.0f,silkStateR=0.0f;
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SakuraShimmerProcessor)
};
