#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* GlowFilterProcessor::createEditor() { return new GlowFilterEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new GlowFilterProcessor(); }
