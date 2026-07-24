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
        versionManager.registerParameter ("key", 0.0f, 1);
        versionManager.registerParameter ("scale", 0.0f, 1);
        versionManager.registerParameter ("strength", 50.0f, 1);
        versionManager.registerParameter ("tune", 440.0f, 1);
        versionManager.registerParameter ("mix", 1.0f, 1);
        versionManager.registerParameter ("gain", 0.0f, 1);
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "key", 1 }, "Key", juce::NormalisableRange<float> (0.0f, 11.0f), 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "scale", 1 }, "Scale", juce::NormalisableRange<float> (0.0f, 6.0f), 0.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "strength", 1 }, "Strength", juce::NormalisableRange<float> (0.0f, 100.0f), 50.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "tune", 1 }, "Tune", juce::NormalisableRange<float> (432.0f, 444.0f), 440.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        rngState_ = ${SEED}u; // seed lock: reproducible renders




        sm_key.reset (apvts.getRawParameterValue ("key")->load(), 10.0f, sampleRate);
        sm_scale.reset (apvts.getRawParameterValue ("scale")->load(), 10.0f, sampleRate);
        sm_strength.reset (apvts.getRawParameterValue ("strength")->load(), 10.0f, sampleRate);
        sm_tune.reset (apvts.getRawParameterValue ("tune")->load(), 10.0f, sampleRate);
        sm_mix.reset (apvts.getRawParameterValue ("mix")->load(), 10.0f, sampleRate);
        sm_gain.reset (apvts.getRawParameterValue ("gain")->load(), 10.0f, sampleRate);

        juce::dsp::ProcessSpec dspSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        for (int i = 0; i < num_bands; ++i)
{
    filters[i].reset();
    current_coeffs[i] = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(getSampleRate(), 1000.0f, 1.0f, 0.0f);
    filters[i].setCoefficients(current_coeffs[i]);
}
active_midi_notes.fill(false);

updateFilterCoefficients(getSampleRate(), 0, 0, 440.0f, 0.0f);

smooth_mix = 0.0f;
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
const float key_param = apvts.getRawParameterValue("key")->load();
const float scale_param = apvts.getRawParameterValue("scale")->load();
const float tune_param = apvts.getRawParameterValue("tune")->load();
const float strength_param = apvts.getRawParameterValue("strength")->load();
const float mix_param = apvts.getRawParameterValue("mix")->load();

const float smoothing_factor = 0.01f;
smooth_mix += smoothing_factor * (mix_param - smooth_mix);

if (key_param != last_key || scale_param != last_scale || tune_param != last_tune || strength_param != last_strength)
{
    updateFilterCoefficients(getSampleRate(), static_cast<int>(key_param), static_cast<int>(scale_param), tune_param, strength_param);
    last_key = key_param;
    last_scale = scale_param;
    last_tune = tune_param;
    last_strength = strength_param;

    // Send updated band data to UI
    juce::var::Array band_data;
    for (int i = 0; i < num_bands; ++i)
    {
        juce::var::Array coeffs;
        for (int c = 0; c < 6; ++c) coeffs.add(current_coeffs[i].coefficients[c]);
        band_data.add(coeffs);
    }
    juce::var::Array active_notes_var;
    for (int i = 0; i < 128; ++i) active_notes_var.add(active_midi_notes[i]);

    juce::var message;
    message.getDynamicObject()->setProperty("type", "updateHarmonic2K");
    message.getDynamicObject()->setProperty("bands", band_data);
    message.getDynamicObject()->setProperty("activeNotes", active_notes_var);
    postMessage(message);
}

for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
{
    auto* channel_data = block.getChannelPointer((int)ch);
    for (size_t i = 0; i < block.getNumSamples(); ++i)
    {
        float dry_sample = channel_data[i];
        float wet_sample = dry_sample;

        for (int band = 0; band < num_bands; ++band)
        {
            wet_sample = filters[band].processSample(wet_sample);
        }
        channel_data[i] = dry_sample * (1.0f - smooth_mix) + wet_sample * smooth_mix;
    }
}
        gainDsp.setGainDecibels (smoothedParam ("gain"));
    gainDsp.process (ctx);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ScopedNoDenormals noDenormals;
        sm_key.setTarget (apvts.getRawParameterValue ("key")->load());
        sm_key.update (getSampleRate());
        sm_scale.setTarget (apvts.getRawParameterValue ("scale")->load());
        sm_scale.update (getSampleRate());
        sm_strength.setTarget (apvts.getRawParameterValue ("strength")->load());
        sm_strength.update (getSampleRate());
        sm_tune.setTarget (apvts.getRawParameterValue ("tune")->load());
        sm_tune.update (getSampleRate());
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
    WoManusSmoothedParameter<float> sm_key;
    WoManusSmoothedParameter<float> sm_scale;
    WoManusSmoothedParameter<float> sm_strength;
    WoManusSmoothedParameter<float> sm_tune;
    WoManusSmoothedParameter<float> sm_mix;
    WoManusSmoothedParameter<float> sm_gain;

    // Zipper-noise-free parameter access. Prefer smoothedParam("id") over raw
    // apvts loads inside audio code; advances one smoothing step per call site.
    inline float smoothedParam (const char* id) noexcept
    {
        if (strcmp (id, "key") == 0) return sm_key.getNextValue();
        if (strcmp (id, "scale") == 0) return sm_scale.getNextValue();
        if (strcmp (id, "strength") == 0) return sm_strength.getNextValue();
        if (strcmp (id, "tune") == 0) return sm_tune.getNextValue();
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
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (50.0f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (440.0f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (1.0f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        break;
    case 1:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (46.7127f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (440.4f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-3.5219f));
        break;
    case 2:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (42.5364f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (439.4517f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (0.6312f));
        break;
    case 3:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (59.4785f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (438.8392f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-1.1823f));
        break;
    case 4:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (55.1238f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (439.255f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-3.8842f));
        break;
    case 5:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (3.3f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (1.8f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (53.7804f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (439.4464f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.7f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-3.7392f));
        break;
    case 6:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (11.0f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (2.3654f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (56.6011f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (433.8696f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.96f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-5.5015f));
        break;
    case 7:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (1.8482f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (2.5671f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (63.1862f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (440.3062f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.5343f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (6.7301f));
        break;
    case 8:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (0.992f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (4.4675f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (8.5303f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (440.0786f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.1652f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-20.615f));
        break;
    case 9:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (7.5692f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (2.1236f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (63.007f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (439.4656f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.4644f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (8.6574f));
        break;
    case 10:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (6.2493f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (5.9601f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (47.8869f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (438.1181f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9638f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (18.9485f));
        break;
    case 11:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (0.2216f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (0.1999f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (99.5417f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (443.4125f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0307f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-20.7609f));
        break;
    case 12:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (10.9123f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (0.0939f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (2.789f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (432.3358f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.0006f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (21.5135f));
        break;
    case 13:
        if (auto* param = apvts.getParameter ("key")) param->setValueNotifyingHost (param->convertTo0to1 (10.7764f));
        if (auto* param = apvts.getParameter ("scale")) param->setValueNotifyingHost (param->convertTo0to1 (5.5654f));
        if (auto* param = apvts.getParameter ("strength")) param->setValueNotifyingHost (param->convertTo0to1 (94.2378f));
        if (auto* param = apvts.getParameter ("tune")) param->setValueNotifyingHost (param->convertTo0to1 (432.8149f));
        if (auto* param = apvts.getParameter ("mix")) param->setValueNotifyingHost (param->convertTo0to1 (0.9869f));
        if (auto* param = apvts.getParameter ("gain")) param->setValueNotifyingHost (param->convertTo0to1 (-20.6398f));
        break;
        default: break;
        }


    }


static constexpr int num_bands = 12; // Reduced number of bands
std::array<juce::dsp::IIR::Coefficients<float>, num_bands> current_coeffs;
std::array<juce::dsp::IIR::Filter<float>, num_bands> filters;

float last_key = 0.0f;
float last_scale = 0.0f;
float last_tune = 440.0f;
float last_strength = 0.0f;

float smooth_mix = 0.0f;

std::array<float, 7> major_scale_intervals = {0.0f, 2.0f, 4.0f, 5.0f, 7.0f, 9.0f, 11.0f};
std::array<float, 7> minor_scale_intervals = {0.0f, 2.0f, 3.0f, 5.0f, 7.0f, 8.0f, 10.0f};
std::array<float, 7> dorian_scale_intervals = {0.0f, 2.0f, 3.0f, 5.0f, 7.0f, 9.0f, 10.0f};
std::array<float, 7> phrygian_scale_intervals = {0.0f, 1.0f, 3.0f, 5.0f, 7.0f, 8.0f, 10.0f};
std::array<float, 7> lydian_scale_intervals = {0.0f, 2.0f, 4.0f, 6.0f, 7.0f, 9.0f, 11.0f};
std::array<float, 7> mixolydian_scale_intervals = {0.0f, 2.0f, 4.0f, 5.0f, 7.0f, 9.0f, 10.0f};
std::array<float, 7> phrygian_dom_scale_intervals = {0.0f, 1.0f, 4.0f, 5.0f, 7.0f, 8.0f, 10.0f};

std::array<bool, 128> active_midi_notes; // For UI piano strip

void updateFilterCoefficients(float sampleRate, int key_idx, int scale_idx, float tune_freq, float strength_param)
{
    active_midi_notes.fill(false);

    const std::array<float, 7>* current_scale_ptr;
    switch (scale_idx)
    {
        case 0: current_scale_ptr = &major_scale_intervals; break;
        case 1: current_scale_ptr = &minor_scale_intervals; break;
        case 2: current_scale_ptr = &dorian_scale_intervals; break;
        case 3: current_scale_ptr = &phrygian_scale_intervals; break;
        case 4: current_scale_ptr = &lydian_scale_intervals; break;
        case 5: current_scale_ptr = &mixolydian_scale_intervals; break;
        case 6: current_scale_ptr = &phrygian_dom_scale_intervals; break;
        default: current_scale_ptr = &major_scale_intervals; break;
    }
    const auto& current_scale_intervals = *current_scale_ptr;

    int band_count = 0;
    // Focus on notes around middle C for the 12 bands
    for (int midi_note = 48; midi_note <= 72; ++midi_note) // C3 to C5 (25 notes, will pick 12)
    {
        if (band_count >= num_bands) break;

        float freq = tune_freq * std::pow(2.0f, (midi_note - 69) / 12.0f);
        if (freq < 20.0f || freq > 20000.0f) continue;

        int note_in_octave = midi_note % 12;
        bool in_scale = false;
        for (float interval : current_scale_intervals)
        {
            if (static_cast<int>(fmodf(note_in_octave - key_idx + 12, 12)) == static_cast<int>(interval))
            {
                in_scale = true;
                break;
            }
        }

        if (in_scale)
        {
            current_coeffs[band_count] = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(sampleRate, freq, 2.0f, juce::Decibels::decibelsToGain(1.5f * strength_param / 100.0f));
            active_midi_notes[midi_note] = true;
        }
        else
        {
            current_coeffs[band_count] = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(sampleRate, freq, 1.5f, juce::Decibels::decibelsToGain(-0.8f * strength_param / 100.0f));
        }
        filters[band_count].setCoefficients(current_coeffs[band_count]);
        band_count++;
    }

    // Fill remaining bands with bypass or neutral filters if not all 12 were used
    for (int i = band_count; i < num_bands; ++i)
    {
        current_coeffs[i] = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(sampleRate, 1000.0f, 1.0f, 0.0f);
        filters[i].setCoefficients(current_coeffs[i]);
    }
}
    juce::dsp::Gain<float> gainDsp;
    TruePeakLimiter truePeakLeft;
    TruePeakLimiter truePeakRight;
    int demoSampleCounter = 0;
    bool demoSilenceActive = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Harmonic2KProcessor)
};
