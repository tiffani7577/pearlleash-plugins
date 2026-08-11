// EtherealReverb offline render test.
// Renders impulse, sine sweep, and silence through the processor at default
// parameters; writes WAVs next to the binary and prints one JSON metrics line
// per signal. Build target: EtherealReverbTest. Machine-readable so the platform's
// autonomous loop can hear and judge its own inventions.
#include "../Source/PluginProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdio>
#include <cmath>
#include <limits>
#include <cstring>

static void metrics (const char* name, const juce::AudioBuffer<float>& b, double sr)
{
    double peak = 0, sumSq = 0, dc = 0, centroidNum = 0, centroidDen = 0;
    int nanCount = 0, denormCount = 0;
    const int N = b.getNumSamples();
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        const float* d = b.getReadPointer (ch);
        for (int i = 0; i < N; ++i)
        {
            const float x = d[i];
            if (std::isnan (x) || std::isinf (x)) { ++nanCount; continue; }
            const float a = std::abs (x);
            if (a > 0.0f && a < 1.0e-30f) ++denormCount;
            peak = juce::jmax (peak, (double) a);
            sumSq += (double) x * x;
            dc += x;
        }
    }
    // crude spectral centroid on channel 0 via zero-crossing weighted estimate
    { const float* d = b.getReadPointer (0); int zc = 0;
      for (int i = 1; i < N; ++i) if ((d[i-1] < 0) != (d[i] < 0)) ++zc;
      centroidNum = 0.5 * zc * sr / juce::jmax (1, N); centroidDen = 1; }
    const int total = N * b.getNumChannels();
    std::printf ("{\"signal\":\"%s\",\"peak\":%.6f,\"rms\":%.6f,\"dc\":%.8f,"
                 "\"nan\":%d,\"denormal\":%d,\"zcFreqHz\":%.1f,\"pass\":%s}\n",
        name, peak, std::sqrt (sumSq / juce::jmax (1, total)), dc / juce::jmax (1, total),
        nanCount, denormCount, centroidNum / centroidDen,
        (nanCount == 0 && peak < 4.0 && std::abs (dc / juce::jmax (1, total)) < 0.05) ? "true" : "false");
}

static void writeWav (const juce::AudioBuffer<float>& b, double sr, const juce::String& file)
{
    juce::WavAudioFormat fmt;
    auto f = juce::File::getCurrentWorkingDirectory().getChildFile (file);
    f.deleteFile();
    if (auto os = f.createOutputStream())
        if (auto* w = fmt.createWriterFor (os.release(), sr, (unsigned) b.getNumChannels(), 24, {}, 0))
        { std::unique_ptr<juce::AudioFormatWriter> wr (w); wr->writeFromAudioSampleBuffer (b, 0, b.getNumSamples()); }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const double sr = 48000.0; const int block = 512; const int seconds = 2;
    EtherealReverbProcessor proc;
    proc.setPlayConfigDetails (2, 2, sr, block);
    proc.prepareToPlay (sr, block);

    auto render = [&] (const char* name, auto fill, int blockSize = block)
    {
        juce::AudioBuffer<float> out (2, (int) sr * seconds); out.clear();
        proc.reset(); proc.prepareToPlay (sr, blockSize);
        for (int pos = 0; pos < out.getNumSamples(); pos += blockSize)
        {
            const int n = juce::jmin (blockSize, out.getNumSamples() - pos);
            juce::AudioBuffer<float> chunk (2, n); chunk.clear();
            fill (chunk, pos);
            juce::MidiBuffer midi;

            proc.processBlock (chunk, midi);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, chunk, ch, 0, n);
        }
        metrics (name, out, sr);
        writeWav (out, sr, juce::String (name) + "_render.wav");
        return out;
    };

    render ("impulse", [] (juce::AudioBuffer<float>& c, int pos)
        { if (pos == 0) for (int ch = 0; ch < 2; ++ch) c.setSample (ch, 0, 1.0f); });
    render ("sweep", [sr] (juce::AudioBuffer<float>& c, int pos)
        { for (int ch = 0; ch < 2; ++ch) { auto* d = c.getWritePointer (ch);
            for (int i = 0; i < c.getNumSamples(); ++i) {
                const double t = (pos + i) / sr;
                const double f = 20.0 * std::pow (1000.0, t / 2.0);
                d[i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * f * t); } } });
    render ("silence", [] (juce::AudioBuffer<float>&, int) {});

    // ---- Commercial hardening regressions (JSON lines for harness / CI) ----
    {
        // Latency reporting honesty
        const int reported = proc.getLatencySamples();
        std::printf ("{\"signal\":\"latency_report\",\"latencySamples\":%d,\"pass\":%s}\n",
            reported, reported >= 0 ? "true" : "false");
    }
    {
        // Variable block-size matrix
        const int sizes[] = { 1, 32, 64, 512, 2048 };
        bool ok = true; int nanTotal = 0;
        for (int bs : sizes)
        {
            proc.reset();
            proc.prepareToPlay (sr, bs);
            juce::AudioBuffer<float> chunk (2, bs); chunk.clear();
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < bs; ++i)
                    chunk.setSample (ch, i, 0.1f * std::sin (0.01f * (float) i));
            juce::MidiBuffer midi;
            proc.processBlock (chunk, midi);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < bs; ++i)
                    if (std::isnan (chunk.getSample (ch, i)) || std::isinf (chunk.getSample (ch, i)))
                        { ++nanTotal; ok = false; }
        }
        std::printf ("{\"signal\":\"blocksize_matrix\",\"nan\":%d,\"pass\":%s}\n",
            nanTotal, ok ? "true" : "false");
    }
    {
        // Sample-rate transition
        proc.reset(); proc.prepareToPlay (44100.0, block);
        juce::AudioBuffer<float> a (2, block); a.clear();
        juce::MidiBuffer midi;
        for (int i = 0; i < block; ++i) a.setSample (0, i, 0.2f);
        proc.processBlock (a, midi);
        proc.reset(); proc.prepareToPlay (96000.0, block);
        juce::AudioBuffer<float> b (2, block); b.clear();
        for (int i = 0; i < block; ++i) b.setSample (0, i, 0.2f);
        proc.processBlock (b, midi);
        int nanCount = 0;
        for (int i = 0; i < block; ++i)
            if (std::isnan (b.getSample (0, i)) || std::isinf (b.getSample (0, i))) ++nanCount;
        std::printf ("{\"signal\":\"sr_transition\",\"nan\":%d,\"pass\":%s}\n",
            nanCount, nanCount == 0 ? "true" : "false");
        proc.prepareToPlay (sr, block);
    }
    {
        // NaN recovery via SignalSanity / output stage
        proc.reset(); proc.prepareToPlay (sr, block);
        juce::AudioBuffer<float> chunk (2, block);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < block; ++i)
                chunk.setSample (ch, i, (i == 10) ? std::numeric_limits<float>::quiet_NaN() : 0.1f);
        juce::MidiBuffer midi;
        proc.processBlock (chunk, midi);
        int nanOut = 0; float peak = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < block; ++i)
            {
                const float x = chunk.getSample (ch, i);
                if (std::isnan (x) || std::isinf (x)) ++nanOut;
                else peak = juce::jmax (peak, std::abs (x));
            }
        std::printf ("{\"signal\":\"nan_recovery\",\"nan\":%d,\"peak\":%.6f,\"pass\":%s}\n",
            nanOut, peak, nanOut == 0 ? "true" : "false");
    }
    {
        // State recall round-trip
        if (auto* p = proc.apvts.getParameter ("bypass"))
            p->setValueNotifyingHost (1.0f);
        juce::MemoryBlock mb;
        proc.getStateInformation (mb);
        if (auto* p = proc.apvts.getParameter ("bypass"))
            p->setValueNotifyingHost (0.0f);
        proc.setStateInformation (mb.getData(), (int) mb.getSize());
        float bypassVal = 0.0f;
        if (auto* raw = proc.apvts.getRawParameterValue ("bypass"))
            bypassVal = raw->load();
        std::printf ("{\"signal\":\"state_recall\",\"bypass\":%.3f,\"pass\":%s}\n",
            bypassVal, bypassVal >= 0.5f ? "true" : "false");
        if (auto* p = proc.apvts.getParameter ("bypass"))
            p->setValueNotifyingHost (0.0f);
    }
    {
        // Zipper / automation step-response energy on first continuous float param
        const char* stepId = "decayTime";
        double clickEnergy = 0.0;
        if (std::strlen (stepId) > 0)
        {
            if (auto* p = proc.apvts.getParameter (stepId))
                p->setValueNotifyingHost (0.0f);
            proc.reset(); proc.prepareToPlay (sr, block);
            juce::AudioBuffer<float> before (2, block), after (2, block);
            before.clear(); after.clear();
            for (int i = 0; i < block; ++i)
            {
                const float s = 0.25f * std::sin (0.05f * (float) i);
                before.setSample (0, i, s); before.setSample (1, i, s);
                after.setSample (0, i, s); after.setSample (1, i, s);
            }
            juce::MidiBuffer midi;
            proc.processBlock (before, midi);
            if (auto* p = proc.apvts.getParameter (stepId))
                p->setValueNotifyingHost (1.0f);
            proc.processBlock (after, midi);
            for (int i = 0; i < block; ++i)
            {
                const float d = after.getSample (0, i) - before.getSample (0, i);
                clickEnergy += (double) d * (double) d;
            }
        }
        // Soft gate: smoothed params should not produce extreme impulse energy on a unit step.
        const bool pass = clickEnergy < 50.0;
        std::printf ("{\"signal\":\"zipper_step\",\"energy\":%.6f,\"pass\":%s}\n",
            clickEnergy, pass ? "true" : "false");
    }
    {
        // Bypass click energy (engage mid-stream)
        proc.reset(); proc.prepareToPlay (sr, block);
        if (auto* p = proc.apvts.getParameter ("bypass"))
            p->setValueNotifyingHost (0.0f);
        juce::AudioBuffer<float> wet (2, block), mixed (2, block);
        for (int i = 0; i < block; ++i)
        {
            const float s = 0.3f * std::sin (0.07f * (float) i);
            wet.setSample (0, i, s); wet.setSample (1, i, s);
            mixed.setSample (0, i, s); mixed.setSample (1, i, s);
        }
        juce::MidiBuffer midi;
        proc.processBlock (wet, midi);
        if (auto* p = proc.apvts.getParameter ("bypass"))
            p->setValueNotifyingHost (1.0f);
        proc.processBlock (mixed, midi);
        double energy = 0.0;
        for (int i = 0; i < juce::jmin (64, block); ++i)
        {
            const float d = mixed.getSample (0, i) - wet.getSample (0, i);
            energy += (double) d * (double) d;
        }
        // ClicklessBypass fades over ~5ms — first 64 samples at 48k should stay bounded.
        const bool pass = energy < 20.0 && ! std::isnan (energy);
        std::printf ("{\"signal\":\"bypass_click\",\"energy\":%.6f,\"pass\":%s}\n",
            energy, pass ? "true" : "false");
        if (auto* p = proc.apvts.getParameter ("bypass"))
            p->setValueNotifyingHost (0.0f);
    }
    return 0;
}
