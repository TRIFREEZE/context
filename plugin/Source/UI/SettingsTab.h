#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Motion.h"

namespace context
{
class ContextAudioProcessor;
}

namespace context::ui
{
/** All settings on one scrolling page (License, Generation, Your track, Storage,
    Interface, Diagnostics, About) with a section index on the left. */
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
    void buildIndex();
    void updateIndexHighlight();
    std::unique_ptr<Content> content;
    struct ScrollWatch : juce::Viewport
    {
        std::function<void()> onScroll;
        void visibleAreaChanged (const juce::Rectangle<int>&) override { if (onScroll) onScroll(); }
    } viewport;
    juce::OwnedArray<juce::TextButton> index;
    motion::Value scroll { 0.0f };
};
} // namespace context::ui
