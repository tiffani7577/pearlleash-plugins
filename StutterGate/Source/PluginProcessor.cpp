#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* StutterGateProcessor::createEditor() { return new StutterGateEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new StutterGateProcessor(); }
