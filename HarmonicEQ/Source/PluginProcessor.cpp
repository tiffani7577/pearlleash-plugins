#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <xmmintrin.h>

juce::AudioProcessorEditor* HarmonicEQProcessor::createEditor() { return new HarmonicEQEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new HarmonicEQProcessor(); }
