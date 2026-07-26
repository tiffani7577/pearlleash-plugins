#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* RhythmGateForgeProcessor::createEditor() { return new RhythmGateForgeEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new RhythmGateForgeProcessor(); }
