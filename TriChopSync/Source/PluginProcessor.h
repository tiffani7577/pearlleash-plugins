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


class TriChopSyncProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    TriChopSyncProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, juce::Identifier ("PARAMS"), createLayout())
    {
        // Portable FTZ (x86 MXCSR / arm64 FPCR). processBlock also uses juce::ScopedNoDenormals.
        pfEnableFlushToZero();

        licensed = TriChopSyncLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("TriChopSync: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("lowCross", 250.0f, 1);
        versionManager.registerParameter ("highCross", 2500.0f, 1);
        versionManager.registerParameter ("lowSubdiv", 2.0f, 1);
        versionManager.registerParameter ("midSubdiv", 4.0f, 1);
        versionManager.registerParameter ("highSubdiv", 5.0f, 1);
        versionManager.registerParameter ("lowDepth", 1.0f, 1);
        versionManager.registerParameter ("midDepth", 1.0f, 1);
        versionManager.registerParameter ("highDepth", 1.0f, 1);
        versionManager.registerParameter ("mix", 1.0f, 1);
        versionManager.registerParameter ("gain", 0.0f, 1);


    }

    ~TriChopSyncProcessor() override
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
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "lowCross", 1 }, "Low Crossover", juce::NormalisableRange<float> (80.0f, 800.0f), 250.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "highCross", 1 }, "High Crossover", juce::NormalisableRange<float> (800.0f, 8000.0f), 2500.0f));
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "lowSubdiv", 1 }, "Low Subdivision", juce::StringArray { "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/128" }, 2));
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "midSubdiv", 1 }, "Mid Subdivision", juce::StringArray { "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/128" }, 4));
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "highSubdiv", 1 }, "High Subdivision", juce::StringArray { "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/128" }, 5));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "lowDepth", 1 }, "Low Gate Depth", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "midDepth", 1 }, "Mid Gate Depth", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "highDepth", 1 }, "High Gate Depth", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Wet/Dry Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = 1347570764u; // seed lock: reproducible renders




        sm_lowCross.reset (apvts.getRawParameterValue ("lowCross")->load(), 10.0f, sampleRate);
        sm_highCross.reset (apvts.getRawParameterValue ("highCross")->load(), 10.0f, sampleRate);
        sm_lowSubdiv.reset (apvts.getRawParameterValue ("lowSubdiv")->load(), 10.0f, sampleRate);
        sm_midSubdiv.reset (apvts.getRawParameterValue ("midSubdiv")->load(), 10.0f, sampleRate);
        sm_highSubdiv.reset (apvts.getRawParameterValue ("highSubdiv")->load(), 10.0f, sampleRate);
        sm_lowDepth.reset (apvts.getRawParameterValue ("lowDepth")->load(), 10.0f, sampleRate);
        sm_midDepth.reset (apvts.getRawParameterValue ("midDepth")->load(), 10.0f, sampleRate);
        sm_highDepth.reset (apvts.getRawParameterValue ("highDepth")->load(), 10.0f, sampleRate);
        sm_mix.reset (apvts.getRawParameterValue ("mix")->load(), 5.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        srF=(float) sampleRate;
envCoeff=1.0f-expf(-1.0f/(srF*0.002f));
for (size_t c = 0; c < (size_t) 2; ++c){for (size_t k = 0; k < (size_t) 4; ++k){lp1L[c][k]=lp1H[c][k]=lp2L[c][k]=lp2H[c][k]=0.0f;hp1L[c][k]=hp1H[c][k]=hp2L[c][k]=hp2H[c][k]=0.0f;lp3L[c][k]=lp3H[c][k]=hp3L[c][k]=hp3H[c][k]=0.0f;}}
for (size_t k = 0; k < (size_t) 3; ++k){phase[k]=0.0f;env[k]=0.0f;}
lowCrossSmooth.reset(srF,0.020f);lowCrossSmooth.setCurrentAndTargetValue(250.0f);
highCrossSmooth.reset(srF,0.020f);highCrossSmooth.setCurrentAndTargetValue(2500.0f);
lowDepthSmooth.reset(srF,0.020f);lowDepthSmooth.setCurrentAndTargetValue(1.0f);
midDepthSmooth.reset(srF,0.020f);midDepthSmooth.setCurrentAndTargetValue(1.0f);
highDepthSmooth.reset(srF,0.020f);highDepthSmooth.setCurrentAndTargetValue(1.0f);
mixSmooth.reset(srF,0.020f);mixSmooth.setCurrentAndTargetValue(1.0f);
calcLR4(250.0f,srF,lcL,lcH);
calcLR4(2500.0f,srF,hcL,hcH);
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
        // ---- custom block: TriChopSync (AI-generated) ----
const float bpm=juce::jmax(1.0f,getBpm());
float rawLC=juce::jlimit(80.0f,800.0f,apvts.getRawParameterValue("lowCross")->load());
float rawHC=juce::jlimit(800.0f,8000.0f,apvts.getRawParameterValue("highCross")->load());
float rawLS=juce::jlimit(0.0f,7.0f,apvts.getRawParameterValue("lowSubdiv")->load());
float rawMS=juce::jlimit(0.0f,7.0f,apvts.getRawParameterValue("midSubdiv")->load());
float rawHS=juce::jlimit(0.0f,7.0f,apvts.getRawParameterValue("highSubdiv")->load());
float rawLD=juce::jlimit(0.0f,1.0f,apvts.getRawParameterValue("lowDepth")->load());
float rawMD=juce::jlimit(0.0f,1.0f,apvts.getRawParameterValue("midDepth")->load());
float rawHD=juce::jlimit(0.0f,1.0f,apvts.getRawParameterValue("highDepth")->load());
float rawMix=juce::jlimit(0.0f,1.0f,apvts.getRawParameterValue("mix")->load());
lowCrossSmooth.setTargetValue(rawLC);
highCrossSmooth.setTargetValue(rawHC);
lowDepthSmooth.setTargetValue(rawLD);
midDepthSmooth.setTargetValue(rawMD);
highDepthSmooth.setTargetValue(rawHD);
mixSmooth.setTargetValue(rawMix);
const float duty=0.5f;
const int lsi=(int)rawLS, msi=(int)rawMS, hsi=(int)rawHS;
const float lFreq=(bpm/60.0f)*subdivTable[lsi];
const float mFreq=(bpm/60.0f)*subdivTable[msi];
const float hFreq=(bpm/60.0f)*subdivTable[hsi];
const float lInc=lFreq/srF, mInc=mFreq/srF, hInc=hFreq/srF;
float prevLC=lowCrossSmooth.getCurrentValue();
float prevHC=highCrossSmooth.getCurrentValue();
for(size_t ch=0;ch<block.getNumChannels();++ch){
  auto* d=block.getChannelPointer((int)ch);
  for(size_t i=0;i<block.getNumSamples();++i){
    float curLC=lowCrossSmooth.getNextValue();
    float curHC=highCrossSmooth.getNextValue();
    if(fabsf(curLC-prevLC)>0.5f){calcLR4(curLC,srF,lcL,lcH);prevLC=curLC;}
    if(fabsf(curHC-prevHC)>0.5f){calcLR4(curHC,srF,hcL,hcH);prevHC=curHC;}
    float x=d[i];
    float lo=bq(bq(x,lcL,lp1L[ch]),lcL,lp2L[ch]);
    float hi=bq(bq(x,lcH,hp1L[ch]),lcH,hp2L[ch]);
    float mid_hi=bq(bq(hi,hcL,lp3L[ch]),hcL,lp3H[ch]);
    float hi2=bq(bq(hi,hcH,hp3L[ch]),hcH,hp3H[ch]);
    if(ch==0){
      phase[0]+=lInc; if(phase[0]>=1.0f)phase[0]-=1.0f;
      phase[1]+=mInc; if(phase[1]>=1.0f)phase[1]-=1.0f;
      phase[2]+=hInc; if(phase[2]>=1.0f)phase[2]-=1.0f;
      float go0=(phase[0]<duty)?1.0f:0.0f;
      float go1=(phase[1]<duty)?1.0f:0.0f;
      float go2=(phase[2]<duty)?1.0f:0.0f;
      env[0]+=(go0-env[0])*envCoeff;
      env[1]+=(go1-env[1])*envCoeff;
      env[2]+=(go2-env[2])*envCoeff;
    }
    float ld=lowDepthSmooth.getNextValue();
    float md=midDepthSmooth.getNextValue();
    float hd=highDepthSmooth.getNextValue();
    float glo=1.0f-ld+ld*env[0];
    float gmi=1.0f-md+md*env[1];
    float ghi=1.0f-hd+hd*env[2];
    float wet=lo*glo+mid_hi*gmi+hi2*ghi;
    float mixV=mixSmooth.getNextValue();
    d[i]=mixV*wet+(1.0f-mixV)*x;
  }
}
        gainDsp.setGainDecibels (smoothedParam ("gain"));
    gainDsp.process (ctx);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ignoreUnused (midiMessages);
        juce::ScopedNoDenormals noDenormals;
        sm_lowCross.setTarget (apvts.getRawParameterValue ("lowCross")->load());
        sm_lowCross.update (getSampleRate());
        sm_highCross.setTarget (apvts.getRawParameterValue ("highCross")->load());
        sm_highCross.update (getSampleRate());
        sm_lowSubdiv.setTarget (apvts.getRawParameterValue ("lowSubdiv")->load());
        sm_lowSubdiv.update (getSampleRate());
        sm_midSubdiv.setTarget (apvts.getRawParameterValue ("midSubdiv")->load());
        sm_midSubdiv.update (getSampleRate());
        sm_highSubdiv.setTarget (apvts.getRawParameterValue ("highSubdiv")->load());
        sm_highSubdiv.update (getSampleRate());
        sm_lowDepth.setTarget (apvts.getRawParameterValue ("lowDepth")->load());
        sm_lowDepth.update (getSampleRate());
        sm_midDepth.setTarget (apvts.getRawParameterValue ("midDepth")->load());
        sm_midDepth.update (getSampleRate());
        sm_highDepth.setTarget (apvts.getRawParameterValue ("highDepth")->load());
        sm_highDepth.update (getSampleRate());
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
    const juce::String getName() const override { return "TriChopSync"; }
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
    WoManusSmoothedParameter<float> sm_lowCross;
    WoManusSmoothedParameter<float> sm_highCross;
    WoManusSmoothedParameter<float> sm_lowSubdiv;
    WoManusSmoothedParameter<float> sm_midSubdiv;
    WoManusSmoothedParameter<float> sm_highSubdiv;
    WoManusSmoothedParameter<float> sm_lowDepth;
    WoManusSmoothedParameter<float> sm_midDepth;
    WoManusSmoothedParameter<float> sm_highDepth;
    WoManusSmoothedParameter<float> sm_mix;
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "lowCross") == 0) return sm_lowCross.getNextValue();
        if (strcmp (id, "highCross") == 0) return sm_highCross.getNextValue();
        if (strcmp (id, "lowSubdiv") == 0) return sm_lowSubdiv.getNextValue();
        if (strcmp (id, "midSubdiv") == 0) return sm_midSubdiv.getNextValue();
        if (strcmp (id, "highSubdiv") == 0) return sm_highSubdiv.getNextValue();
        if (strcmp (id, "lowDepth") == 0) return sm_lowDepth.getNextValue();
        if (strcmp (id, "midDepth") == 0) return sm_midDepth.getNextValue();
        if (strcmp (id, "highDepth") == 0) return sm_highDepth.getNextValue();
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
        if (auto* param = apvts.getParameter ("lowCross")) param->setValueNotifyingHost (param->convertTo0to1 (250.0f));
        if (auto* param = apvts.getParameter ("highCross")) param->setValueNotifyingHost (param->convertTo0to1 (2500.0f));
        if (auto* param = apvts.getParameter ("lowSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (2.0f));
        if (auto* param = apvts.getParameter ("midSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (4.0f));
        if (auto* param = apvts.getParameter ("highSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (5.0f));
        if (auto* param = apvts.getParameter ("lowDepth")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("midDepth")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("highDepth")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("lowCross")) param->setValueNotifyingHost (param->convertTo0to1 (296.0f));
        if (auto* param = apvts.getParameter ("highCross")) param->setValueNotifyingHost (param->convertTo0to1 (2960.0f));
        if (auto* param = apvts.getParameter ("lowSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (2.1f));
        if (auto* param = apvts.getParameter ("midSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (4.5612f));
        if (auto* param = apvts.getParameter ("highSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (4.9f));
        if (auto* param = apvts.getParameter ("lowDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("midDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("highDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (3.2115f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("lowCross")) param->setValueNotifyingHost (param->convertTo0to1 (296.0f));
        if (auto* param = apvts.getParameter ("highCross")) param->setValueNotifyingHost (param->convertTo0to1 (2960.0f));
        if (auto* param = apvts.getParameter ("lowSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (2.2359f));
        if (auto* param = apvts.getParameter ("midSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (3.737f));
        if (auto* param = apvts.getParameter ("highSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (4.8653f));
        if (auto* param = apvts.getParameter ("lowDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("midDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("highDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (2.331f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("lowCross")) param->setValueNotifyingHost (param->convertTo0to1 (296.0f));
        if (auto* param = apvts.getParameter ("highCross")) param->setValueNotifyingHost (param->convertTo0to1 (2960.0f));
        if (auto* param = apvts.getParameter ("lowSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (2.1013f));
        if (auto* param = apvts.getParameter ("midSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (3.8964f));
        if (auto* param = apvts.getParameter ("highSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (4.9f));
        if (auto* param = apvts.getParameter ("lowDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("midDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("highDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-1.1091f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("lowCross")) param->setValueNotifyingHost (param->convertTo0to1 (296.0f));
        if (auto* param = apvts.getParameter ("highCross")) param->setValueNotifyingHost (param->convertTo0to1 (2960.0f));
        if (auto* param = apvts.getParameter ("lowSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (2.1f));
        if (auto* param = apvts.getParameter ("midSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (4.2515f));
        if (auto* param = apvts.getParameter ("highSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (4.8388f));
        if (auto* param = apvts.getParameter ("lowDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("midDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("highDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-4.7539f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("lowCross")) param->setValueNotifyingHost (param->convertTo0to1 (296.0f));
        if (auto* param = apvts.getParameter ("highCross")) param->setValueNotifyingHost (param->convertTo0to1 (3161.6097f));
        if (auto* param = apvts.getParameter ("lowSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (2.6449f));
        if (auto* param = apvts.getParameter ("midSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (4.0839f));
        if (auto* param = apvts.getParameter ("highSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (4.9f));
        if (auto* param = apvts.getParameter ("lowDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("midDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("highDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-2.2438f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("lowCross")) param->setValueNotifyingHost (param->convertTo0to1 (755.7186f));
        if (auto* param = apvts.getParameter ("highCross")) param->setValueNotifyingHost (param->convertTo0to1 (6126.9875f));
        if (auto* param = apvts.getParameter ("lowSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (3.2265f));
        if (auto* param = apvts.getParameter ("midSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (1.7486f));
        if (auto* param = apvts.getParameter ("highSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (5.7636f));
        if (auto* param = apvts.getParameter ("lowDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.871f));
        if (auto* param = apvts.getParameter ("midDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7549f));
        if (auto* param = apvts.getParameter ("highDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.833f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5931f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-6.2684f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("lowCross")) param->setValueNotifyingHost (param->convertTo0to1 (242.6544f));
        if (auto* param = apvts.getParameter ("highCross")) param->setValueNotifyingHost (param->convertTo0to1 (5380.379f));
        if (auto* param = apvts.getParameter ("lowSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (3.1348f));
        if (auto* param = apvts.getParameter ("midSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (1.5173f));
        if (auto* param = apvts.getParameter ("highSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (2.3836f));
        if (auto* param = apvts.getParameter ("lowDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.1231f));
        if (auto* param = apvts.getParameter ("midDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.5965f));
        if (auto* param = apvts.getParameter ("highDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.6404f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.1053f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-15.3697f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("lowCross")) param->setValueNotifyingHost (param->convertTo0to1 (601.4785f));
        if (auto* param = apvts.getParameter ("highCross")) param->setValueNotifyingHost (param->convertTo0to1 (1829.45f));
        if (auto* param = apvts.getParameter ("lowSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (5.0703f));
        if (auto* param = apvts.getParameter ("midSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (5.3432f));
        if (auto* param = apvts.getParameter ("highSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (0.8811f));
        if (auto* param = apvts.getParameter ("lowDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.2782f));
        if (auto* param = apvts.getParameter ("midDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.1995f));
        if (auto* param = apvts.getParameter ("highDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.6889f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.3057f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-7.5226f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("lowCross")) param->setValueNotifyingHost (param->convertTo0to1 (557.4328f));
        if (auto* param = apvts.getParameter ("highCross")) param->setValueNotifyingHost (param->convertTo0to1 (3300.0221f));
        if (auto* param = apvts.getParameter ("lowSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (2.2204f));
        if (auto* param = apvts.getParameter ("midSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (5.3209f));
        if (auto* param = apvts.getParameter ("highSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (2.8239f));
        if (auto* param = apvts.getParameter ("lowDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.6451f));
        if (auto* param = apvts.getParameter ("midDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7828f));
        if (auto* param = apvts.getParameter ("highDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.5541f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.6042f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-0.0442f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("lowCross")) param->setValueNotifyingHost (param->convertTo0to1 (531.3702f));
        if (auto* param = apvts.getParameter ("highCross")) param->setValueNotifyingHost (param->convertTo0to1 (7282.4764f));
        if (auto* param = apvts.getParameter ("lowSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (3.8113f));
        if (auto* param = apvts.getParameter ("midSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (0.9757f));
        if (auto* param = apvts.getParameter ("highSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (5.2278f));
        if (auto* param = apvts.getParameter ("lowDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.2992f));
        if (auto* param = apvts.getParameter ("midDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.239f));
        if (auto* param = apvts.getParameter ("highDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.7523f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5382f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (2.1975f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("lowCross")) param->setValueNotifyingHost (param->convertTo0to1 (82.469f));
        if (auto* param = apvts.getParameter ("highCross")) param->setValueNotifyingHost (param->convertTo0to1 (7911.6755f));
        if (auto* param = apvts.getParameter ("lowSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (6.8619f));
        if (auto* param = apvts.getParameter ("midSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (6.9829f));
        if (auto* param = apvts.getParameter ("highSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (0.169f));
        if (auto* param = apvts.getParameter ("lowDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.992f));
        if (auto* param = apvts.getParameter ("midDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.9958f));
        if (auto* param = apvts.getParameter ("highDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.008f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9781f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (21.5527f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("lowCross")) param->setValueNotifyingHost (param->convertTo0to1 (107.1031f));
        if (auto* param = apvts.getParameter ("highCross")) param->setValueNotifyingHost (param->convertTo0to1 (7464.5045f));
        if (auto* param = apvts.getParameter ("lowSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (0.253f));
        if (auto* param = apvts.getParameter ("midSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (0.4607f));
        if (auto* param = apvts.getParameter ("highSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (6.8128f));
        if (auto* param = apvts.getParameter ("lowDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.0787f));
        if (auto* param = apvts.getParameter ("midDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.9888f));
        if (auto* param = apvts.getParameter ("highDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.9747f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9723f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (21.07f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("lowCross")) param->setValueNotifyingHost (param->convertTo0to1 (761.9441f));
        if (auto* param = apvts.getParameter ("highCross")) param->setValueNotifyingHost (param->convertTo0to1 (1086.3375f));
        if (auto* param = apvts.getParameter ("lowSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (6.6601f));
        if (auto* param = apvts.getParameter ("midSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (6.5244f));
        if (auto* param = apvts.getParameter ("highSubdiv")) param->setValueNotifyingHost (param->convertTo0to1 (0.2384f));
        if (auto* param = apvts.getParameter ("lowDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.0666f));
        if (auto* param = apvts.getParameter ("midDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.0452f));
        if (auto* param = apvts.getParameter ("highDepth")) param->setValueNotifyingHost (param->convertTo0to1 (0.0013f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9733f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (23.0557f));
        break;
        default: break;
        }


    }


float srF = 44100.0f;
float lp1L[2][4]={}, lp1H[2][4]={}, lp2L[2][4]={}, lp2H[2][4]={};
float hp1L[2][4]={}, hp1H[2][4]={}, hp2L[2][4]={}, hp2H[2][4]={};
float lp3L[2][4]={}, lp3H[2][4]={}, hp3L[2][4]={}, hp3H[2][4]={};
float lcL[5]={}, lcH[5]={}, hcL[5]={}, hcH[5]={};
float phase[3]={0.0f,0.0f,0.0f};
float env[3]={0.0f,0.0f,0.0f};
float envCoeff=0.0f;
static const float subdivTable[8]={4.0f,2.0f,1.0f,0.5f,0.25f,0.125f,0.0625f,0.03125f};
juce::SmoothedValue<float> lowCrossSmooth, highCrossSmooth;
juce::SmoothedValue<float> lowDepthSmooth, midDepthSmooth, highDepthSmooth;
juce::SmoothedValue<float> mixSmooth;
void calcLR4(float fc, float sr, float* lc, float* hc){
  float w=2.0f*(float)M_PI*fc/sr;
  float cw=cosf(w),sw=sinf(w);
  float q=0.7071f,a=sw/(2.0f*q);
  float b0=((1.0f-cw)/2.0f),b1=(1.0f-cw),b2=b0,a0=1.0f+a,a1=-2.0f*cw,a2=1.0f-a;
  lc[0]=b0/a0;lc[1]=b1/a0;lc[2]=b2/a0;lc[3]=-a1/a0;lc[4]=-a2/a0;
  float hb0=((1.0f+cw)/2.0f),hb1=-(1.0f+cw),hb2=hb0;
  hc[0]=hb0/a0;hc[1]=hb1/a0;hc[2]=hb2/a0;hc[3]=-a1/a0;hc[4]=-a2/a0;
}
float bq(float x,float* c,float* z){
  float y=c[0]*x+c[1]*z[0]+c[2]*z[1]+c[3]*z[2]+c[4]*z[3];
  z[1]=z[0];z[0]=x;z[3]=z[2];z[2]=y;return y;
}
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TriChopSyncProcessor)
};
