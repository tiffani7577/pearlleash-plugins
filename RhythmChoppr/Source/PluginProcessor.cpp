#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* RhythmChopprProcessor::createEditor() { return new RhythmChopprEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new RhythmChopprProcessor(); }
