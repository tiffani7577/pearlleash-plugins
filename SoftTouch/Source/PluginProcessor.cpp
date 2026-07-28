#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* SoftTouchProcessor::createEditor() { return new SoftTouchEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SoftTouchProcessor(); }
