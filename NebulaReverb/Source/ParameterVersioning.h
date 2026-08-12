#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
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

    /** Legacy path — prefer migrate + applyMissingParameterDefaults + single replaceState. */
    void setStateInformation (juce::AudioProcessorValueTreeState& apvts,
                              const void* data, int sizeInBytes)
    {
        auto tree = juce::ValueTree::readFromData (data, (size_t) sizeInBytes);
        if (! tree.isValid())
            return;

        const int savedVersion = (int) tree.getProperty ("plugforgeVersion", 0);
        applyMissingParameterDefaults (apvts, tree, savedVersion);
        apvts.replaceState (tree);
    }

    /**
     * Fill APVTS defaults for parameters introduced after savedVersion, and ensure
     * ValueTree children exist for every registered id before replaceState.
     * Does not call replaceState itself (caller owns a single replace).
     */
    void applyMissingParameterDefaults (juce::AudioProcessorValueTreeState& apvts,
                                        juce::ValueTree& tree,
                                        int savedVersion) const
    {
        for (const auto& param : defaults)
        {
            if (param.introducedInVersion > savedVersion)
            {
                if (auto* p = apvts.getParameter (param.paramId))
                    p->setValueNotifyingHost (p->convertTo0to1 (param.defaultValue));
            }

            bool found = false;
            for (int i = 0; i < tree.getNumChildren(); ++i)
            {
                auto child = tree.getChild (i);
                if (child.hasType ("PARAM") && child.getProperty ("id").toString() == param.paramId)
                {
                    found = true;
                    break;
                }
                if (child.getProperty ("id").toString() == param.paramId)
                {
                    found = true;
                    break;
                }
            }
            if (! found)
            {
                juce::ValueTree child ("PARAM");
                child.setProperty ("id", param.paramId, nullptr);
                child.setProperty ("value", param.defaultValue, nullptr);
                tree.appendChild (child, nullptr);
            }
        }
    }

private:
    std::vector<ParameterDefault> defaults;
};
