#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* SakuraShimmerProcessor::createEditor() { return new SakuraShimmerEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SakuraShimmerProcessor(); }
