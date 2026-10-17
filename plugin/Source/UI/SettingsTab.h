#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace context
{
class ContextAudioProcessor;
}

namespace context::ui
{
/** All settings in one scrolling page: License, Generation, Track Context,
    Storage, Preview, Diagnostics, About. */
class SettingsTab : public juce::Component
{
public:
    explicit SettingsTab (ContextAudioProcessor&);
    ~SettingsTab() override;

    void refresh();
    std::function<void()> onRunSetupAgain;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class Content;
    std::unique_ptr<Content> content;
    juce::Viewport viewport;
};
} // namespace context::ui
