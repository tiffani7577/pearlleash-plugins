#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* KawaiiShimmerVerbProcessor::createEditor() { return new KawaiiShimmerVerbEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new KawaiiShimmerVerbProcessor(); }
