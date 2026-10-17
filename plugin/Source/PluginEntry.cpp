#include "PluginProcessor.h"

// Entry point used by the JUCE plugin wrappers (VST3 / Standalone).
// Kept in its own file so the test executable can link the processor without it.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new context::ContextAudioProcessor();
}
