#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <xmmintrin.h>

juce::AudioProcessorEditor* Harmonic2KProcessor::createEditor() { return new Harmonic2KEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new Harmonic2KProcessor(); }
