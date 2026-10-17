#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BuildInfo.h"

namespace context
{
ContextAudioProcessor::ContextAudioProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

ContextAudioProcessor::~ContextAudioProcessor() = default;

const juce::String ContextAudioProcessor::getName() const { return build::productName; }

void ContextAudioProcessor::prepareToPlay (double, int) {}
void ContextAudioProcessor::releaseResources() {}

bool ContextAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void ContextAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    // Transparent passthrough: input channels are already in the buffer.
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());
}

juce::AudioProcessorEditor* ContextAudioProcessor::createEditor()
{
    return new ContextAudioProcessorEditor (*this);
}

void ContextAudioProcessor::getStateInformation (juce::MemoryBlock&) {}
void ContextAudioProcessor::setStateInformation (const void*, int) {}
} // namespace context
