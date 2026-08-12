#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* NebulaReverbProcessor::createEditor() { return new NebulaReverbEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new NebulaReverbProcessor(); }
