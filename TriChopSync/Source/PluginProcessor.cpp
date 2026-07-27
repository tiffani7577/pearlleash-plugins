#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* TriChopSyncProcessor::createEditor() { return new TriChopSyncEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TriChopSyncProcessor(); }
