#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* EchoChamberProcessor::createEditor() { return new EchoChamberEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new EchoChamberProcessor(); }
