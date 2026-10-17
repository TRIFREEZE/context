#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace context
{
class ContextAudioProcessor;

class ContextAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit ContextAudioProcessorEditor (ContextAudioProcessor&);
    ~ContextAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    ContextAudioProcessor& processor;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ContextAudioProcessorEditor)
};
} // namespace context
