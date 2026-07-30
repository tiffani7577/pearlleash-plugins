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


class WarmTapeDelayProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    WarmTapeDelayProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, juce::Identifier ("PARAMS"), createLayout())
    {
        // Portable FTZ (x86 MXCSR / arm64 FPCR). processBlock also uses juce::ScopedNoDenormals.
        pfEnableFlushToZero();

        licensed = WarmTapeDelayLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("WarmTapeDelay: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("delayTime", 250.0f, 1);
        versionManager.registerParameter ("feedback", 0.5f, 1);
        versionManager.registerParameter ("tone", 0.4f, 1);
        versionManager.registerParameter ("drive", 1.5f, 1);
        versionManager.registerParameter ("wowFlutter", 0.003f, 1);
        versionManager.registerParameter ("mix", 0.5f, 1);
        versionManager.registerParameter ("gain", 0.0f, 1);


    }

    ~WarmTapeDelayProcessor() override
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
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "delayTime", 1 }, "Time", juce::NormalisableRange<float> (1.0f, 1200.0f), 250.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "feedback", 1 }, "Feedback", juce::NormalisableRange<float> (0.0f, 0.95f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "tone", 1 }, "Tone", juce::NormalisableRange<float> (0.01f, 1.0f), 0.4f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "drive", 1 }, "Saturation", juce::NormalisableRange<float> (0.5f, 4.0f), 1.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "wowFlutter", 1 }, "Wow & Flutter", juce::NormalisableRange<float> (0.0f, 0.02f), 0.003f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = 1347570764u; // seed lock: reproducible renders




        sm_delayTime.reset (apvts.getRawParameterValue ("delayTime")->load(), 10.0f, sampleRate);
        sm_feedback.reset (apvts.getRawParameterValue ("feedback")->load(), 10.0f, sampleRate);
        sm_tone.reset (apvts.getRawParameterValue ("tone")->load(), 10.0f, sampleRate);
        sm_drive.reset (apvts.getRawParameterValue ("drive")->load(), 10.0f, sampleRate);
        sm_wowFlutter.reset (apvts.getRawParameterValue ("wowFlutter")->load(), 10.0f, sampleRate);
        sm_mix.reset (apvts.getRawParameterValue ("mix")->load(), 5.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        srF = (float) sampleRate;
bufL.fill(0.0f); bufR.fill(0.0f);
writePos = 0;
toneStateL = 0.0f; toneStateR = 0.0f;
wowPhase = 0.0f; flutterPhase = 0.0f;
noiseState = 0.0f; fbStateL = 0.0f; fbStateR = 0.0f;
rmsIn = 0.0f; rmsOut = 0.0f; agcGain = 1.0f;
delaySmoothed.reset(srF, 0.020f); delaySmoothed.setCurrentAndTargetValue(250.0f);
mixSmoothed.reset(srF, 0.020f); mixSmoothed.setCurrentAndTargetValue(0.5f);
feedbackSmoothed.reset(srF, 0.020f); feedbackSmoothed.setCurrentAndTargetValue(0.5f);
toneSmoothed.reset(srF, 0.020f); toneSmoothed.setCurrentAndTargetValue(0.4f);
driveSmoothed.reset(srF, 0.020f); driveSmoothed.setCurrentAndTargetValue(1.5f);
wowFlutterSmoothed.reset(srF, 0.020f); wowFlutterSmoothed.setCurrentAndTargetValue(0.003f);
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
        // ---- custom block: WarmTapeDelay (AI-generated) ----
const float bpm = juce::jmax(1.0f, getBpm());
const float samplesPerBeat = (60.0f / bpm) * srF;
(void)samplesPerBeat;
float rawDelay = apvts.getRawParameterValue("delayTime")->load();
rawDelay = juce::jlimit(1.0f, 1200.0f, rawDelay);
float rawMix = apvts.getRawParameterValue("mix")->load();
rawMix = juce::jlimit(0.0f, 1.0f, rawMix);
float rawFb = apvts.getRawParameterValue("feedback")->load();
rawFb = juce::jlimit(0.0f, 0.95f, rawFb);
float rawTone = apvts.getRawParameterValue("tone")->load();
rawTone = juce::jlimit(0.01f, 1.0f, rawTone);
float rawDrive = apvts.getRawParameterValue("drive")->load();
rawDrive = juce::jlimit(0.5f, 4.0f, rawDrive);
float rawWow = apvts.getRawParameterValue("wowFlutter")->load();
rawWow = juce::jlimit(0.0f, 0.02f, rawWow);
delaySmoothed.setTargetValue(rawDelay);
mixSmoothed.setTargetValue(rawMix);
feedbackSmoothed.setTargetValue(rawFb);
toneSmoothed.setTargetValue(rawTone);
driveSmoothed.setTargetValue(rawDrive);
wowFlutterSmoothed.setTargetValue(rawWow);
const float wowRate = 0.5f;
const float flutterRate = 7.0f;
const float twoPi = 6.28318530718f;
size_t numCh = block.getNumChannels();
size_t numSamples = block.getNumSamples();
auto* dL = block.getChannelPointer(0);
auto* dR = (numCh > 1) ? block.getChannelPointer(1) : block.getChannelPointer(0);
for (size_t i = 0; i < numSamples; ++i) {
  float delaySamples = delaySmoothed.getNextValue() * 0.001f * srF;
  float fbGain = feedbackSmoothed.getNextValue();
  float toneNorm = toneSmoothed.getNextValue();
  float drive = driveSmoothed.getNextValue();
  float wowDepth = wowFlutterSmoothed.getNextValue();
  float mixVal = mixSmoothed.getNextValue();
  noiseState = noiseState * 0.9999f + ((float)rand() / (float)RAND_MAX * 2.0f - 1.0f) * 0.0001f;
  float warpOffset = wowDepth * srF * std::sin(twoPi * wowPhase);
  warpOffset += (wowDepth * 0.5f) * srF * std::sin(twoPi * flutterPhase + noiseState * 3.14159f);
  wowPhase += wowRate / srF; if (wowPhase >= 1.0f) wowPhase -= 1.0f;
  flutterPhase += flutterRate / srF; if (flutterPhase >= 1.0f) flutterPhase -= 1.0f;
  float readDist = juce::jlimit(1.0f, (float)(kMaxDelay - 2), delaySamples - warpOffset);
  float readPosF = (float)writePos - readDist;
  while (readPosF < 0.0f) readPosF += (float)kMaxDelay;
  int r0 = (int)readPosF % kMaxDelay;
  int r1 = (r0 + 1) % kMaxDelay;
  int r2 = (r0 + 2) % kMaxDelay;
  int rm1 = (r0 - 1 + kMaxDelay) % kMaxDelay;
  float frac = readPosF - std::floor(readPosF);
  float c0 = -frac*(frac-1.0f)*(frac-2.0f)/6.0f;
  float c1 = (frac+1.0f)*(frac-1.0f)*(frac-2.0f)/2.0f;
  float c2 = -(frac+1.0f)*frac*(frac-2.0f)/2.0f;
  float c3 = (frac+1.0f)*frac*(frac-1.0f)/6.0f;
  float delOutL = c0*bufL[rm1] + c1*bufL[r0] + c2*bufL[r1] + c3*bufL[r2];
  float delOutR = c0*bufR[rm1] + c1*bufR[r0] + c2*bufR[r1] + c3*bufR[r2];
  float toneFreq = juce::jlimit(200.0f, 18000.0f, toneNorm * 8000.0f + 200.0f);
  float g = std::tan(3.14159265f * toneFreq / srF);
  float toneCoeff = g / (1.0f + g);
  float satInL = delOutL * drive;
  float satInR = delOutR * drive;
  float prevSatInL = fbStateL;
  float prevSatInR = fbStateR;
  fbStateL = satInL;
  fbStateR = satInR;
  float adaaL, adaaR;
  float diffL = satInL - prevSatInL;
  float diffR = satInR - prevSatInR;
  if (std::abs(diffL) > 1e-6f) {
    float F1 = std::log(std::cosh(satInL));
    float F0 = std::log(std::cosh(prevSatInL));
    adaaL = (F1 - F0) / diffL;
  } else {
    adaaL = std::tanh(0.5f * (satInL + prevSatInL));
  }
  if (std::abs(diffR) > 1e-6f) {
    float F1 = std::log(std::cosh(satInR));
    float F0 = std::log(std::cosh(prevSatInR));
    adaaR = (F1 - F0) / diffR;
  } else {
    adaaR = std::tanh(0.5f * (satInR + prevSatInR));
  }
  rmsIn = 0.9999f * rmsIn + 0.0001f * delOutL * delOutL;
  rmsOut = 0.9999f * rmsOut + 0.0001f * adaaL * adaaL;
  float rmsInSqrt = std::sqrt(juce::jmax(1e-12f, rmsIn));
  float rmsOutSqrt = std::sqrt(juce::jmax(1e-12f, rmsOut));
  float targetAgc = rmsInSqrt / rmsOutSqrt;
  agcGain = 0.9995f * agcGain + 0.0005f * juce::jlimit(0.1f, 2.0f, targetAgc);
  adaaL *= agcGain;
  adaaR *= agcGain;
  toneStateL = toneCoeff * adaaL + (1.0f - toneCoeff) * toneStateL;
  toneStateR = toneCoeff * adaaR + (1.0f - toneCoeff) * toneStateR;
  float fbL = toneStateL * fbGain;
  float fbR = toneStateR * fbGain;
  if (!std::isfinite(fbL)) fbL = 0.0f;
  if (!std::isfinite(fbR)) fbR = 0.0f;
  fbL += 1e-25f;
  fbR += 1e-25f;
  float inL = dL[i];
  float inR = dR[i];
  bufL[writePos] = inL + fbL;
  bufR[writePos] = inR + fbR;
  writePos = (writePos + 1) % kMaxDelay;
  float decorrL = delOutL + delOutR * 0.03f;
  float decorrR = delOutR + delOutL * 0.03f;
  float wetL = decorrL;
  float wetR = decorrR;
  float dryL = inL;
  float dryR = inR;
  dL[i] = dryL * (1.0f - mixVal) + wetL * mixVal;
  dR[i] = dryR * (1.0f - mixVal) + wetR * mixVal;
}
        gainDsp.setGainDecibels (smoothedParam ("gain"));
    gainDsp.process (ctx);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ignoreUnused (midiMessages);
        juce::ScopedNoDenormals noDenormals;
        sm_delayTime.setTarget (apvts.getRawParameterValue ("delayTime")->load());
        sm_delayTime.update (getSampleRate());
        sm_feedback.setTarget (apvts.getRawParameterValue ("feedback")->load());
        sm_feedback.update (getSampleRate());
        sm_tone.setTarget (apvts.getRawParameterValue ("tone")->load());
        sm_tone.update (getSampleRate());
        sm_drive.setTarget (apvts.getRawParameterValue ("drive")->load());
        sm_drive.update (getSampleRate());
        sm_wowFlutter.setTarget (apvts.getRawParameterValue ("wowFlutter")->load());
        sm_wowFlutter.update (getSampleRate());
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
    const juce::String getName() const override { return "WarmTapeDelay"; }
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
    WoManusSmoothedParameter<float> sm_delayTime;
    WoManusSmoothedParameter<float> sm_feedback;
    WoManusSmoothedParameter<float> sm_tone;
    WoManusSmoothedParameter<float> sm_drive;
    WoManusSmoothedParameter<float> sm_wowFlutter;
    WoManusSmoothedParameter<float> sm_mix;
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "delayTime") == 0) return sm_delayTime.getNextValue();
        if (strcmp (id, "feedback") == 0) return sm_feedback.getNextValue();
        if (strcmp (id, "tone") == 0) return sm_tone.getNextValue();
        if (strcmp (id, "drive") == 0) return sm_drive.getNextValue();
        if (strcmp (id, "wowFlutter") == 0) return sm_wowFlutter.getNextValue();
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
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (250.0f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (0.4f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (1.5f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.003f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (360.7f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.4448f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (0.4281f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (1.55f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.006f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5727f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-0.0052f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (360.7f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.5607f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (0.3498f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (1.55f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.006f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5455f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (4.5378f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (360.7f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.5031f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (0.4935f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (1.7985f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.006f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.519f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (2.9964f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (360.7f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.4645f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (0.3657f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (1.55f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.006f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4267f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (3.2938f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (360.7f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.5621f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (0.4728f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (1.55f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.006f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5109f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-2.7994f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (304.1634f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.5167f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (0.3602f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (3.4069f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.0191f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7276f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-3.4391f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (565.1863f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.6449f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (0.3584f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (1.8672f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.0024f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.6382f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-10.3048f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (700.4074f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.5054f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (0.118f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (2.2736f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.0141f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.1758f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-5.9709f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (679.957f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.6825f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (0.4402f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (2.1199f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.0063f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5057f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (12.2193f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (835.3994f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.7085f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (0.9085f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (3.4968f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.019f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7434f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-18.6371f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (28.5787f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.8959f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (0.046f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (3.8327f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.0192f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.046f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-20.3395f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (59.6651f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.0644f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (0.0688f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (0.7331f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.0014f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0149f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-23.7901f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (80.4808f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.9108f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (0.0791f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (3.8369f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.0015f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0279f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-21.4028f));
        break;
        default: break;
        }


    }


static constexpr int kMaxDelay = 192001;
std::array<float,192001> bufL{};
std::array<float,192001> bufR{};
int writePos = 0;
float srF = 44100.0f;
juce::SmoothedValue<float> delaySmoothed;
juce::SmoothedValue<float> mixSmoothed;
juce::SmoothedValue<float> feedbackSmoothed;
juce::SmoothedValue<float> toneSmoothed;
juce::SmoothedValue<float> driveSmoothed;
juce::SmoothedValue<float> wowFlutterSmoothed;
float toneStateL = 0.0f;
float toneStateR = 0.0f;
float wowPhase = 0.0f;
float flutterPhase = 0.0f;
float noiseState = 0.0f;
float fbStateL = 0.0f;
float fbStateR = 0.0f;
float rmsIn = 0.0f;
float rmsOut = 0.0f;
float agcGain = 1.0f;
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WarmTapeDelayProcessor)
};
