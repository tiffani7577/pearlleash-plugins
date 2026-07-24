#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <cstring>
#include <cmath>
#include <xmmintrin.h>
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


class Harmonic2KProcessor : public juce::AudioProcessor
{
public:
    Harmonic2KProcessor()
        : AudioProcessor (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "PARAMS", createLayout())
    {
        _mm_setcsr (_mm_getcsr() | 0x8040);

        licensed = Harmonic2KLicense::checkLicense();
        if (! licensed)
            juce::Logger::writeToLog ("Harmonic2K: unlicensed copy — enter your serial to activate.");
        versionManager.registerParameter ("rootnote", 0.0f, 1);
        versionManager.registerParameter ("scaletype", 0.0f, 1);
        versionManager.registerParameter ("tune", 440.0f, 1);
        versionManager.registerParameter ("strength", 100.0f, 1);
        versionManager.registerParameter ("mix", 100.0f, 1);
        versionManager.registerParameter ("gain", 0.0f, 1);
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "rootnote", 1 }, "Root Note", juce::NormalisableRange<float> (0.0f, 11.0f), 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "scaletype", 1 }, "Scale Type", juce::NormalisableRange<float> (0.0f, 6.0f), 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "tune", 1 }, "Master Tune", juce::NormalisableRange<float> (432.0f, 444.0f), 440.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "strength", 1 }, "Harmonic Strength", juce::NormalisableRange<float> (0.0f, 100.0f), 100.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 100.0f), 100.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = ${SEED}u; // seed lock: reproducible renders




        sm_rootnote.reset (apvts.getRawParameterValue ("rootnote")->load(), 10.0f, sampleRate);
        sm_scaletype.reset (apvts.getRawParameterValue ("scaletype")->load(), 10.0f, sampleRate);
        sm_tune.reset (apvts.getRawParameterValue ("tune")->load(), 10.0f, sampleRate);
        sm_strength.reset (apvts.getRawParameterValue ("strength")->load(), 10.0f, sampleRate);
        sm_mix.reset (apvts.getRawParameterValue ("mix")->load(), 10.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
          currentSampleRate = (float) getSampleRate();
  for (int i = 0; i < maxNumFilters; ++i) {
      filterCoefficients[i] = juce::dsp::IIR::Coefficients<float>::makePeakFilter(currentSampleRate, 1000.0f, 1.0f, 1.0f);
      filters[i].setCoefficients(*filterCoefficients[i]);
      filters[i].reset();
  }
  smoothedStrength = apvts.getRawParameterValue("strength")->load();
  strengthSmoothingCoeff = 1.0f - std::exp(-1.0f / (0.005f * currentSampleRate)); // 5ms smoothing time
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
        // ---- custom block: Harmonic2K (AI-generated) ----
  const int rootNote = static_cast<int>(apvts.getRawParameterValue("rootnote")->load());
  const int scaleType = static_cast<int>(apvts.getRawParameterValue("scaletype")->load());
  const float tune = apvts.getRawParameterValue("tune")->load();
  const float targetStrength = apvts.getRawParameterValue("strength")->load();
  const float mix = apvts.getRawParameterValue("mix")->load() / 100.0f;

  if (rootNote != lastRootNote || scaleType != lastScaleType || tune != lastTune || targetStrength != lastStrength) {
      updateFilters(rootNote, scaleType, tune, targetStrength);
      lastRootNote = rootNote;
      lastScaleType = scaleType;
      lastTune = tune;
      lastStrength = targetStrength;
  }

  for (size_t ch = 0; ch < block.getNumChannels(); ++ch) {
      auto* d = block.getChannelPointer ((int) ch);
      for (size_t i = 0; i < block.getNumSamples(); ++i) {
          float drySample = d[i];

          // Smooth strength parameter
          smoothedStrength += strengthSmoothingCoeff * (targetStrength - smoothedStrength);

          float wetSample = drySample;
          for (int f = 0; f < maxNumFilters; ++f) {
              wetSample = filters[f].processSample(wetSample);
          }

          d[i] = drySample * (1.0f - mix) + wetSample * mix;
      }
  }
        gainDsp.setGainDecibels (smoothedParam ("gain"));
    gainDsp.process (ctx);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ScopedNoDenormals noDenormals;
        sm_rootnote.setTarget (apvts.getRawParameterValue ("rootnote")->load());
        sm_rootnote.update (getSampleRate());
        sm_scaletype.setTarget (apvts.getRawParameterValue ("scaletype")->load());
        sm_scaletype.update (getSampleRate());
        sm_tune.setTarget (apvts.getRawParameterValue ("tune")->load());
        sm_tune.update (getSampleRate());
        sm_strength.setTarget (apvts.getRawParameterValue ("strength")->load());
        sm_strength.update (getSampleRate());
        sm_mix.setTarget (apvts.getRawParameterValue ("mix")->load());
        sm_mix.update (getSampleRate());
        sm_gain.setTarget (apvts.getRawParameterValue ("gain")->load());
        sm_gain.update (getSampleRate());
        analyzer.pushBuffer (buffer);



        processChain (juce::dsp::AudioBlock<float> (buffer));
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
    const juce::String getName() const override { return "Harmonic2K"; }
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
    WoManusSmoothedParameter<float> sm_rootnote;
    WoManusSmoothedParameter<float> sm_scaletype;
    WoManusSmoothedParameter<float> sm_tune;
    WoManusSmoothedParameter<float> sm_strength;
    WoManusSmoothedParameter<float> sm_mix;
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "rootnote") == 0) return sm_rootnote.getNextValue();
        if (strcmp (id, "scaletype") == 0) return sm_scaletype.getNextValue();
        if (strcmp (id, "tune") == 0) return sm_tune.getNextValue();
        if (strcmp (id, "strength") == 0) return sm_strength.getNextValue();
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
        if (auto* param = apvts.getParameter ("rootnote")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("scaletype")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (440.0f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (100.0f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (100.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("rootnote")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaletype")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (439.6055f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-3.5219f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("rootnote")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaletype")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (439.1044f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.6312f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("rootnote")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaletype")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (440.4f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-1.1823f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("rootnote")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaletype")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (440.4f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-3.8842f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("rootnote")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scaletype")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (440.4f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (70.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-3.7392f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("rootnote")) param->setValueNotifyingHost (param->convertTo0to1 (11.0f));
        if (auto* param = apvts.getParameter ("scaletype")) param->setValueNotifyingHost (param->convertTo0to1 (2.3654f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (438.7921f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (15.5797f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (96.0019f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-5.5015f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("rootnote")) param->setValueNotifyingHost (param->convertTo0to1 (1.8482f));
        if (auto* param = apvts.getParameter ("scaletype")) param->setValueNotifyingHost (param->convertTo0to1 (2.5671f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (439.5823f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (69.2187f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (53.4258f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (6.7301f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("rootnote")) param->setValueNotifyingHost (param->convertTo0to1 (0.992f));
        if (auto* param = apvts.getParameter ("scaletype")) param->setValueNotifyingHost (param->convertTo0to1 (4.4675f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (433.0236f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (67.3214f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (16.5204f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-20.615f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("rootnote")) param->setValueNotifyingHost (param->convertTo0to1 (7.5692f));
        if (auto* param = apvts.getParameter ("scaletype")) param->setValueNotifyingHost (param->convertTo0to1 (2.1236f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (439.5608f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (62.2135f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (46.4434f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (8.6574f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("rootnote")) param->setValueNotifyingHost (param->convertTo0to1 (6.2493f));
        if (auto* param = apvts.getParameter ("scaletype")) param->setValueNotifyingHost (param->convertTo0to1 (5.9601f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (437.7464f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (50.9839f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (96.3835f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (18.9485f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("rootnote")) param->setValueNotifyingHost (param->convertTo0to1 (0.2216f));
        if (auto* param = apvts.getParameter ("scaletype")) param->setValueNotifyingHost (param->convertTo0to1 (0.1999f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (443.945f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (95.104f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (3.0723f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-20.7609f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("rootnote")) param->setValueNotifyingHost (param->convertTo0to1 (10.9123f));
        if (auto* param = apvts.getParameter ("scaletype")) param->setValueNotifyingHost (param->convertTo0to1 (0.0939f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (432.3347f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (2.7985f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0586f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (21.5135f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("rootnote")) param->setValueNotifyingHost (param->convertTo0to1 (10.7764f));
        if (auto* param = apvts.getParameter ("scaletype")) param->setValueNotifyingHost (param->convertTo0to1 (5.5654f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (443.3085f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (6.7908f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (98.6913f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-20.6398f));
        break;
        default: break;
        }


    }


  static constexpr int maxNumFilters = 24;
  std::array<juce::dsp::IIR::Filter<float>, maxNumFilters> filters;
  std::array<juce::dsp::IIR::Coefficients<float>::Ptr, maxNumFilters> filterCoefficients;

  float currentSampleRate = 0.0f;

  std::array<float, 12> majorScale = { 0, 2, 4, 5, 7, 9, 11 }; // MIDI intervals from root
  std::array<float, 12> naturalMinorScale = { 0, 2, 3, 5, 7, 8, 10 };
  std::array<float, 12> dorianScale = { 0, 2, 3, 5, 7, 9, 10 };
  std::array<float, 12> phrygianScale = { 0, 1, 3, 5, 7, 8, 10 };
  std::array<float, 12> lydianScale = { 0, 2, 4, 6, 7, 9, 11 };
  std::array<float, 12> mixolydianScale = { 0, 2, 4, 5, 7, 9, 10 };
  std::array<float, 12> phrygianDominantScale = { 0, 1, 4, 5, 7, 8, 10 };

  int lastRootNote = -1;
  int lastScaleType = -1;
  float lastTune = -1.0f;
  float lastStrength = -1.0f;

  // Smoothing for strength parameter
  float smoothedStrength = 0.0f;
  float strengthSmoothingCoeff = 0.0f;

  // Pre-calculated MIDI note frequencies for 20Hz to 20kHz range
  std::array<float, 128> midiNoteFrequencies;

  void calculateMidiFrequencies(float tuneA4) {
      for (int i = 0; i < 128; ++i) {
          midiNoteFrequencies[i] = tuneA4 * std::pow(2.0f, (i - 69.0f) / 12.0f);
      }
  }

  void updateFilters(int rootNote, int scaleType, float tuneA4, float strength) {
      if (currentSampleRate <= 0.0f) return;

      std::array<float, 12> currentScale;
      switch (scaleType) {
          case 0: currentScale = majorScale; break;
          case 1: currentScale = naturalMinorScale; break;
          case 2: currentScale = dorianScale; break;
          case 3: currentScale = phrygianScale; break;
          case 4: currentScale = lydianScale; break;
          case 5: currentScale = mixolydianScale; break;
          case 6: currentScale = phrygianDominantScale; break;
          default: currentScale = majorScale; break;
      }

      calculateMidiFrequencies(tuneA4);

      std::vector<float> inKeyFrequencies;
      std::vector<float> outOfKeyFrequencies;

      // Iterate through octaves (MIDI notes 21 to 108 covers ~27.5Hz to ~4186Hz, extending slightly beyond)
      // We'll filter frequencies within 20Hz to 20kHz
      for (int midiNote = 21; midiNote <= 108; ++midiNote) {
          float freq = midiNoteFrequencies[midiNote];
          if (freq < 20.0f || freq > 20000.0f) continue;

          int noteInOctave = (midiNote - rootNote) % 12;
          if (noteInOctave < 0) noteInOctave += 12;

          bool isInKey = false;
          for (float interval : currentScale) {
              if (static_cast<int>(interval) == noteInOctave) {
                  isInKey = true;
                  break;
              }
          }

          if (isInKey) {
              inKeyFrequencies.push_back(freq);
          } else {
              outOfKeyFrequencies.push_back(freq);
          }
      }

      int filterIndex = 0;

      // Apply boost filters for in-key frequencies
      for (float freq : inKeyFrequencies) {
          if (filterIndex >= maxNumFilters) break;
          filterCoefficients[filterIndex] = juce::dsp::IIR::Coefficients<float>::makePeakFilter(
              currentSampleRate, freq, 2.0f, juce::Decibels::decibelsToGain(1.5f * strength / 100.0f));
          filters[filterIndex].setCoefficients(*filterCoefficients[filterIndex]);
          filterIndex++;
      }

      // Apply cut filters for out-of-key frequencies
      for (float freq : outOfKeyFrequencies) {
          if (filterIndex >= maxNumFilters) break;
          filterCoefficients[filterIndex] = juce::dsp::IIR::Coefficients<float>::makePeakFilter(
              currentSampleRate, freq, 1.5f, juce::Decibels::decibelsToGain(-0.8f * strength / 100.0f));
          filters[filterIndex].setCoefficients(*filterCoefficients[filterIndex]);
          filterIndex++;
      }

      // Disable remaining filters if any
      for (; filterIndex < maxNumFilters; ++filterIndex) {
          filterCoefficients[filterIndex] = juce::dsp::IIR::Coefficients<float>::makePeakFilter(
              currentSampleRate, 1000.0f, 1.0f, juce::Decibels::decibelsToGain(0.0f)); // Flat response
          filters[filterIndex].setCoefficients(*filterCoefficients[filterIndex]);
      }
  }
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Harmonic2KProcessor)
};
