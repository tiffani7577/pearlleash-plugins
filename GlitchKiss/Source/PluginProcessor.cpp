#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* GlitchKissProcessor::createEditor() { return new GlitchKissEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new GlitchKissProcessor(); }
