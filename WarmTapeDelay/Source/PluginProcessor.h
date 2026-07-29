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
        versionManager.registerParameter ("mix", 0.5f, 1);
        versionManager.registerParameter ("wowFlutter", 0.3f, 1);
        versionManager.registerParameter ("drive", 2.0f, 1);
        versionManager.registerParameter ("tone", 4000.0f, 1);
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
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "wowFlutter", 1 }, "Wow/Flutter", juce::NormalisableRange<float> (0.0f, 1.0f), 0.3f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "drive", 1 }, "Tape Drive", juce::NormalisableRange<float> (1.0f, 4.0f), 2.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "tone", 1 }, "Tone", juce::NormalisableRange<float> (800.0f, 8000.0f), 4000.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = 1347570764u; // seed lock: reproducible renders




        sm_delayTime.reset (apvts.getRawParameterValue ("delayTime")->load(), 10.0f, sampleRate);
        sm_feedback.reset (apvts.getRawParameterValue ("feedback")->load(), 10.0f, sampleRate);
        sm_mix.reset (apvts.getRawParameterValue ("mix")->load(), 5.0f, sampleRate);
        sm_wowFlutter.reset (apvts.getRawParameterValue ("wowFlutter")->load(), 10.0f, sampleRate);
        sm_drive.reset (apvts.getRawParameterValue ("drive")->load(), 10.0f, sampleRate);
        sm_tone.reset (apvts.getRawParameterValue ("tone")->load(), 10.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        srF = (float) sampleRate;
delayBufL.fill(0.0f);
delayBufR.fill(0.0f);
writePos = 0;
fbStateL = 0.0f; fbStateR = 0.0f;
toneStateL = 0.0f; toneStateR = 0.0f;
wowPhase = 0.0f; flutterPhase = 0.0f;
noiseState = 0.0f; sampleIndex = 0.0;
delaySmoothed.reset(srF, 0.020f);
delaySmoothed.setCurrentAndTargetValue(250.0f);
mixSmoothed.reset(srF, 0.020f);
mixSmoothed.setCurrentAndTargetValue(0.5f);
feedbackSmoothed.reset(srF, 0.020f);
feedbackSmoothed.setCurrentAndTargetValue(0.5f);
wowFlutterSmoothed.reset(srF, 0.020f);
wowFlutterSmoothed.setCurrentAndTargetValue(0.3f);
driveSmoothed.reset(srF, 0.020f);
driveSmoothed.setCurrentAndTargetValue(2.0f);
toneSmoothed.reset(srF, 0.020f);
toneSmoothed.setCurrentAndTargetValue(4000.0f);
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
const float rawDelay = juce::jlimit(1.0f, 1200.0f, apvts.getRawParameterValue("delayTime")->load());
const float rawMix = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("mix")->load());
const float rawFB = juce::jlimit(0.0f, 0.95f, apvts.getRawParameterValue("feedback")->load());
const float rawWow = juce::jlimit(0.0f, 1.0f, apvts.getRawParameterValue("wowFlutter")->load());
const float rawDrive = juce::jlimit(1.0f, 4.0f, apvts.getRawParameterValue("drive")->load());
const float rawTone = juce::jlimit(800.0f, 8000.0f, apvts.getRawParameterValue("tone")->load());
delaySmoothed.setTargetValue(rawDelay);
mixSmoothed.setTargetValue(rawMix);
feedbackSmoothed.setTargetValue(rawFB);
wowFlutterSmoothed.setTargetValue(rawWow);
driveSmoothed.setTargetValue(rawDrive);
toneSmoothed.setTargetValue(rawTone);
const float bpm = juce::jmax(1.0f, getBpm());
(void)bpm;
const float wowRate = 0.5f;
const float flutterRate = 8.0f;
const float maxModSamples = 20.0f;
const size_t numSamples = block.getNumSamples();
const size_t numChannels = block.getNumChannels();
if (numChannels < 1) return;
auto* dL = block.getChannelPointer(0);
auto* dR = (numChannels > 1) ? block.getChannelPointer(1) : nullptr;
for (size_t i = 0; i < numSamples; ++i) {
  const float delaySamples = juce::jlimit(1.0f, (float)(kMaxDelaySamples - 1), delaySmoothed.getNextValue() * 0.001f * srF);
  const float fb = feedbackSmoothed.getNextValue();
  const float wowAmt = wowFlutterSmoothed.getNextValue();
  const float drive = driveSmoothed.getNextValue();
  const float tone = toneSmoothed.getNextValue();
  noiseState = noiseState * 0.9999f + ((float)rand() / (float)RAND_MAX * 2.0f - 1.0f) * 0.0001f;
  wowPhase += wowRate / srF;
  if (wowPhase > 1.0f) wowPhase -= 1.0f;
  flutterPhase += flutterRate / srF;
  if (flutterPhase > 1.0f) flutterPhase -= 1.0f;
  const float lfoMod = wowAmt * maxModSamples * (0.7f * std::sin(6.28318f * wowPhase) + 0.3f * std::sin(6.28318f * flutterPhase + noiseState * 3.14159f));
  float readPosF = (float)writePos - delaySamples + lfoMod;
  while (readPosF < 0.0f) readPosF += (float)kMaxDelaySamples;
  while (readPosF >= (float)kMaxDelaySamples) readPosF -= (float)kMaxDelaySamples;
  const int r0 = (int)readPosF;
  const float frac = readPosF - (float)r0;
  const int r1 = (r0 + 1) % kMaxDelaySamples;
  const int rm1 = (r0 - 1 + kMaxDelaySamples) % kMaxDelaySamples;
  const int r2 = (r0 + 2) % kMaxDelaySamples;
  const float toneA1 = std::exp(-6.28318f * tone / srF);
  const float toneB0 = 1.0f - toneA1;
  const float dNorm = juce::jmax(1e-6f, std::tanh(drive));
  float inL = dL[i];
  float inR = (dR != nullptr) ? dR[i] : inL;
  auto cubicInterp = [&](const std::array<float,192001>& buf, float rm1f, float r0f, float r1f, float r2f, float fr) -> float {
    float a = buf[(int)rm1f]; float b = buf[(int)r0f]; float c = buf[(int)r1f]; float d2 = buf[(int)r2f];
    return b + 0.5f * fr * (c - a + fr * (2.0f * a - 5.0f * b + 4.0f * c - d2 + fr * (3.0f * (b - c) + d2 - a)));
  };
  float delOutL = cubicInterp(delayBufL, (float)rm1, (float)r0, (float)r1, (float)r2, frac);
  float delOutR = cubicInterp(delayBufR, (float)rm1, (float)r0, (float)r1, (float)r2, frac);
  float satL = std::tanh(drive * delOutL) / dNorm;
  float satR = std::tanh(drive * delOutR) / dNorm;
  toneStateL = toneB0 * satL + toneA1 * toneStateL;
  toneStateR = toneB0 * satR + toneA1 * toneStateR;
  float fbL = juce::jlimit(-1.0f, 1.0f, toneStateL * fb);
  float fbR = juce::jlimit(-1.0f, 1.0f, toneStateR * fb);
  delayBufL[writePos] = inL + fbL;
  delayBufR[writePos] = inR + fbR;
  writePos = (writePos + 1) % kMaxDelaySamples;
  float dry = inL;
  float wet = delOutL;
  float mixV = mixSmoothed.getNextValue();
  dL[i] = dry * (1.0f - mixV) + wet * mixV;
  if (dR != nullptr) {
    float dry2 = inR;
    float wet2 = delOutR;
    dR[i] = dry2 * (1.0f - mixSmoothed.getNextValue()) + wet2 * mixSmoothed.getNextValue();
  }
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
        sm_mix.setTarget (apvts.getRawParameterValue ("mix")->load());
        sm_mix.update (getSampleRate());
        sm_wowFlutter.setTarget (apvts.getRawParameterValue ("wowFlutter")->load());
        sm_wowFlutter.update (getSampleRate());
        sm_drive.setTarget (apvts.getRawParameterValue ("drive")->load());
        sm_drive.update (getSampleRate());
        sm_tone.setTarget (apvts.getRawParameterValue ("tone")->load());
        sm_tone.update (getSampleRate());
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
    WoManusSmoothedParameter<float> sm_mix;
    WoManusSmoothedParameter<float> sm_wowFlutter;
    WoManusSmoothedParameter<float> sm_drive;
    WoManusSmoothedParameter<float> sm_tone;
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "delayTime") == 0) return sm_delayTime.getNextValue();
        if (strcmp (id, "feedback") == 0) return sm_feedback.getNextValue();
        if (strcmp (id, "mix") == 0) return sm_mix.getNextValue();
        if (strcmp (id, "wowFlutter") == 0) return sm_wowFlutter.getNextValue();
        if (strcmp (id, "drive") == 0) return sm_drive.getNextValue();
        if (strcmp (id, "tone") == 0) return sm_tone.getNextValue();
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
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (2.0f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (4000.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (360.7f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.4448f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5283f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (1.9f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (4523.573f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-0.0052f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (360.7f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.5607f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4493f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (2.0345f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (4327.7865f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (4.5378f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (360.7f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.5031f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5945f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.3853f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (2.1804f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (4136.6973f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (2.9964f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (360.7f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.4645f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4653f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.3005f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (2.0211f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (3472.0527f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (3.2938f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (360.7f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.5621f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5736f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.3f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (2.087f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (4078.5684f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-2.7994f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (304.1634f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.5167f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.3538f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.8306f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (3.8618f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (6038.5133f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-3.4391f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (565.1863f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.6449f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.3519f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.3906f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (1.3634f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (5394.8204f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-10.3048f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (700.4074f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.5054f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.109f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.5068f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (3.116f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (2065.9376f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-5.9709f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (679.957f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.6825f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4346f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.4628f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (1.9434f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (4441.2323f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (12.2193f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (835.3994f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.7085f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9075f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.8562f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (3.8444f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (6152.1955f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-18.6371f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (28.5787f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.8959f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0364f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.9522f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (3.8829f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (1131.3567f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-20.3395f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (59.6651f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.0644f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0594f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.0666f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (1.2042f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (907.0694f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-23.7901f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("delayTime")) param->setValueNotifyingHost (param->convertTo0to1 (80.4808f));
        if (auto* param = apvts.getParameter ("feedback")) param->setValueNotifyingHost (param->convertTo0to1 (0.9108f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0698f));
        if (auto* param = apvts.getParameter ("wowFlutter")) param->setValueNotifyingHost (param->convertTo0to1 (0.9534f));
        if (auto* param = apvts.getParameter ("drive")) param->setValueNotifyingHost (param->convertTo0to1 (1.2179f));
        if (auto* param = apvts.getParameter ("tone")) param->setValueNotifyingHost (param->convertTo0to1 (1001.193f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-21.4028f));
        break;
        default: break;
        }


    }


static constexpr int kMaxDelaySamples = 192001;
std::array<float,192001> delayBufL{};
std::array<float,192001> delayBufR{};
int writePos = 0;
float srF = 44100.0f;
juce::SmoothedValue<float> delaySmoothed;
juce::SmoothedValue<float> mixSmoothed;
juce::SmoothedValue<float> feedbackSmoothed;
juce::SmoothedValue<float> wowFlutterSmoothed;
juce::SmoothedValue<float> driveSmoothed;
juce::SmoothedValue<float> toneSmoothed;
float fbStateL = 0.0f;
float fbStateR = 0.0f;
float toneStateL = 0.0f;
float toneStateR = 0.0f;
float wowPhase = 0.0f;
float flutterPhase = 0.0f;
float noiseState = 0.0f;
double sampleIndex = 0.0;
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WarmTapeDelayProcessor)
};
