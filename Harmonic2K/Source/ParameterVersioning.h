#pragma once
#include <JuceHeader.h>
#include <vector>

class ParameterVersioning
{
public:
    static constexpr int CURRENT_VERSION = 1;

    struct ParameterDefault
    {
        juce::String paramId;
        float defaultValue;
        int introducedInVersion;
    };

    void registerParameter (const juce::String& paramId, float defaultValue, int version = 1)
    {
        defaults.push_back ({ paramId, defaultValue, version });
    }

    void getStateInformation (juce::AudioProcessorValueTreeState& apvts, juce::MemoryBlock& destData)
    {
        auto state = apvts.copyState();
        state.setProperty ("plugforgeVersion", CURRENT_VERSION, nullptr);
        juce::MemoryOutputStream stream (destData, false);
        state.writeToStream (stream);
    }

    void setStateInformation (juce::AudioProcessorValueTreeState& apvts,
                              const void* data, int sizeInBytes)
    {
        auto tree = juce::ValueTree::readFromData (data, (size_t) sizeInBytes);
        if (! tree.isValid())
            return;

        const int savedVersion = tree.getProperty ("plugforgeVersion", 0);

        for (auto& param : defaults)
        {
            if (param.introducedInVersion > savedVersion)
            {
                if (auto* p = apvts.getParameter (param.paramId))
                    p->setValueNotifyingHost (p->convertTo0to1 (param.defaultValue));
            }
        }

        apvts.replaceState (tree);
    }

private:
    std::vector<ParameterDefault> defaults;
};
