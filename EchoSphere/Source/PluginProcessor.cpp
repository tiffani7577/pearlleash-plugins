#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* EchoSphereProcessor::createEditor() { return new EchoSphereEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new EchoSphereProcessor(); }
