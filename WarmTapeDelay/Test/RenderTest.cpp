// WarmTapeDelay offline render test.
// Renders impulse, sine sweep, and silence through the processor at default
// parameters; writes WAVs next to the binary and prints one JSON metrics line
// per signal. Build target: WarmTapeDelayTest. Machine-readable so the platform's
// autonomous loop can hear and judge its own inventions.
#include "../Source/PluginProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdio>
#include <cmath>

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
    WarmTapeDelayProcessor proc;
    proc.setPlayConfigDetails (2, 2, sr, block);
    proc.prepareToPlay (sr, block);

    auto render = [&] (const char* name, auto fill)
    {
        juce::AudioBuffer<float> out (2, (int) sr * seconds); out.clear();
        proc.reset(); proc.prepareToPlay (sr, block);
        for (int pos = 0; pos < out.getNumSamples(); pos += block)
        {
            const int n = juce::jmin (block, out.getNumSamples() - pos);
            juce::AudioBuffer<float> chunk (2, n); chunk.clear();
            fill (chunk, pos);
            juce::MidiBuffer midi;

            proc.processBlock (chunk, midi);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, chunk, ch, 0, n);
        }
        metrics (name, out, sr);
        writeWav (out, sr, juce::String (name) + "_render.wav");
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
    return 0;
}
