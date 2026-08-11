#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* CelestialReverbProcessor::createEditor() { return new CelestialReverbEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new CelestialReverbProcessor(); }
