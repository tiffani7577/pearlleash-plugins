#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* VelvetAscentProcessor::createEditor() { return new VelvetAscentEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VelvetAscentProcessor(); }
