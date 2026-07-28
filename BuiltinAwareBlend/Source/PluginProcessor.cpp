#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* BuiltinAwareBlendProcessor::createEditor() { return new BuiltinAwareBlendEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new BuiltinAwareBlendProcessor(); }
