#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "BuildInfo.h"

namespace context
{
ContextAudioProcessorEditor::ContextAudioProcessorEditor (ContextAudioProcessor& p)
    : juce::AudioProcessorEditor (p), processor (p)
{
    setSize (760, 640);
}

ContextAudioProcessorEditor::~ContextAudioProcessorEditor() = default;

void ContextAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff121316));
    g.setColour (juce::Colours::white);
    g.setFont (28.0f);
    g.drawText (juce::String (build::productName).toUpperCase(), getLocalBounds(), juce::Justification::centred);
}

void ContextAudioProcessorEditor::resized() {}
} // namespace context
