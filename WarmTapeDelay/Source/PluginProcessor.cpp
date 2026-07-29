#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* WarmTapeDelayProcessor::createEditor() { return new WarmTapeDelayEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new WarmTapeDelayProcessor(); }
