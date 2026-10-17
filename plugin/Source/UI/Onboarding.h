#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace context
{
class ContextAudioProcessor;
}

namespace context::ui
{
/** First-run flow: 1) activate Context, 2) connect the local generation bridge
    (or pick the offline Mock Provider). Never asks for provider credentials. */
class Onboarding : public juce::Component
{
public:
    explicit Onboarding (ContextAudioProcessor&);

    void refresh();
    std::function<void()> onFinished;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void finish();
    int step() const;

    ContextAudioProcessor& processor;
    bool skippedLicense = false;

    juce::TextEditor key, address;
    juce::TextButton activate { "Activate" }, skip { "Skip for now" }, test { "Test Connection" },
                     useMock { "Use Mock Provider (offline test)" }, done { "Finish" };
    juce::Label message;
    juce::Rectangle<int> card;
};
} // namespace context::ui
