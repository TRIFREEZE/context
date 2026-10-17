#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "UI/Theme.h"
#include "UI/Motion.h"
#include "UI/Sheet.h"
#include "UI/Workspace.h"
#include "UI/HistoryTab.h"
#include "UI/SettingsTab.h"
#include "UI/Onboarding.h"

namespace context
{
class ContextAudioProcessor;

/** The plugin window. Owns only UI: it can be opened/closed at any time without
    affecting running jobs, which live in the processor's controllers. */
class ContextAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                          public ui::SheetHost,
                                          private juce::ChangeListener,
                                          private juce::Timer
{
public:
    explicit ContextAudioProcessorEditor (ContextAudioProcessor&);
    ~ContextAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    void showTab (const juce::String& id);
    void showShortcuts();

    void presentSheet (std::unique_ptr<ui::Sheet>) override;
    void dismissSheet (ui::Sheet*) override;
    bool hasSheet() const { return sheet != nullptr; }
    void closeSheet() { if (sheet != nullptr) dismissSheet (sheet.get()); }
    ui::Workspace& getWorkspace() { return workspace; }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refreshAll();
    void refreshUndoButtons();
    void openOnboarding();
    juce::Component* tabComponent (const juce::String& id);

    ContextAudioProcessor& processor;
    ui::ContextLookAndFeel lookAndFeel;          // declared first: outlives all children
    juce::TooltipWindow tooltips { this, 600 };
    std::unique_ptr<juce::VBlankAttachment> frameClock;

    juce::TextButton generateTabButton { "Create" }, historyTabButton { "History" }, settingsTabButton { "Settings" };
    ui::IconButton undoButton { ui::IconButton::Icon::Undo, "Undo (Ctrl+Z)" };
    ui::IconButton redoButton { ui::IconButton::Icon::Redo, "Redo (Ctrl+Shift+Z)" };
    ui::IconButton keysButton { ui::IconButton::Icon::Keys, "Keyboard shortcuts (?)" };
    ui::Workspace workspace;
    ui::HistoryTab historyTab;
    ui::SettingsTab settingsTab;
    std::unique_ptr<ui::Onboarding> onboarding;
    std::unique_ptr<ui::Sheet> sheet;

    juce::String currentTab = "generate";
    juce::Rectangle<int> statusPill, licenseChip;
    ui::motion::Rect tabUnderline;               // slides between tabs
    ui::motion::Value tabFade { 1.0f };          // the incoming tab fades in
    juce::String lastStatusText;
    int ticks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ContextAudioProcessorEditor)
};
} // namespace context
