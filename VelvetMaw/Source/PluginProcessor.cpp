#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* VelvetMawProcessor::createEditor() { return new VelvetMawEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VelvetMawProcessor(); }
