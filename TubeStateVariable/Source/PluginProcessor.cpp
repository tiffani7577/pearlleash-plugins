#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* TubeStateVariableProcessor::createEditor() { return new TubeStateVariableEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TubeStateVariableProcessor(); }
