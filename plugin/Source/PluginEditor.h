#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "UI/Theme.h"
#include "UI/GenerateTab.h"
#include "UI/HistoryTab.h"
#include "UI/SettingsTab.h"
#include "UI/Onboarding.h"

namespace context
{
class ContextAudioProcessor;

/** The plugin window. Owns only UI: it can be opened/closed at any time without
    affecting running jobs, which live in the processor's controllers. */
class ContextAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                          private juce::ChangeListener,
                                          private juce::Timer
{
public:
    explicit ContextAudioProcessorEditor (ContextAudioProcessor&);
    ~ContextAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

    void showTab (const juce::String& id);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refreshAll();
    void openOnboarding();

    ContextAudioProcessor& processor;
    ui::ContextLookAndFeel lookAndFeel;          // declared first: outlives all children
    juce::TooltipWindow tooltips { this, 600 };

    juce::TextButton generateTabButton { "Generate" }, historyTabButton { "History" }, settingsTabButton { "Settings" };
    ui::GenerateTab generateTab;
    ui::HistoryTab historyTab;
    ui::SettingsTab settingsTab;
    std::unique_ptr<ui::Onboarding> onboarding;

    juce::String currentTab = "generate";
    juce::Rectangle<int> statusPill, licenseChip;
    int ticks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ContextAudioProcessorEditor)
};
} // namespace context
