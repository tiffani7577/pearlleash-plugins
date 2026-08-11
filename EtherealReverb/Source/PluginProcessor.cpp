#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* EtherealReverbProcessor::createEditor() { return new EtherealReverbEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new EtherealReverbProcessor(); }
