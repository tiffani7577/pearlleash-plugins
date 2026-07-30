#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* EtherBloomProcessor::createEditor() { return new EtherBloomEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new EtherBloomProcessor(); }
