#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

/**
 * AudioReactiveKnob — LookAndFeel_V4 rotary with audio-level glow.
 * Call setLevel() from the UI timer with RMS dB (-60..0 → glow 0..1).
 * WoManus elite UI builtin — PearlLeash Plugins internal.
 */
class AudioReactiveKnob : public juce::LookAndFeel_V4
{
public:
    void setLevel (float rmsDb)
    {
        glowAmount = juce::jlimit (0.0f, 1.0f, juce::jmap (rmsDb, -60.0f, 0.0f, 0.0f, 1.0f));
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y,
                           int width, int height, float sliderPos,
                           float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider& slider) override
    {
        juce::ignoreUnused (slider);

        const auto radius = static_cast<float> (juce::jmin (width, height)) * 0.5f - 8.0f;
        const auto centreX = static_cast<float> (x) + static_cast<float> (width) * 0.5f;
        const auto centreY = static_cast<float> (y) + static_cast<float> (height) * 0.5f;
        const auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

        // Glow under the knob body (audio-reactive)
        if (glowAmount > 0.01f)
        {
            g.setColour (juce::Colour (0xFF00FFAA).withAlpha (glowAmount * 0.4f));
            g.fillEllipse (centreX - radius - 4.0f, centreY - radius - 4.0f,
                           (radius + 4.0f) * 2.0f, (radius + 4.0f) * 2.0f);
        }

        g.setColour (juce::Colour (0xFF1A1A1A));
        g.fillEllipse (centreX - radius, centreY - radius, radius * 2.0f, radius * 2.0f);

        juce::Path arc;
        arc.addCentredArc (centreX, centreY, radius - 4.0f, radius - 4.0f,
                           0.0f, rotaryStartAngle, angle, true);
        g.setColour (juce::Colour (0xFF00FFAA));
        g.strokePath (arc, juce::PathStrokeType (3.0f,
                                                 juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));

        juce::Path pointer;
        pointer.addEllipse (-3.0f, -radius + 2.0f, 6.0f, 6.0f);
        pointer.applyTransform (juce::AffineTransform::rotation (angle)
                                    .translated (centreX, centreY));
        g.fillPath (pointer);
    }

private:
    float glowAmount = 0.0f;
};
