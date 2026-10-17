#pragma once

#include "Controls.h"
#include "Generation/GenerationModels.h"

namespace context { class ContextAudioProcessor; }

namespace context::ui
{
/** Drag handle that hands the DAW the processed export (what you hear). */
class DragToDawPill : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<juce::File()> onDragOut;
    std::function<void()> onClickedWithoutDrag;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override { dragging = false; }
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

private:
    bool dragging = false;
};

/** Audition transport and result actions. */
class TransportBar : public juce::Component
{
public:
    explicit TransportBar (ContextAudioProcessor&);

    void refresh();
    void tick();
    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void (const ProviderError&)> onError;

private:
    void saveAs();
    juce::File exportOrNotify();

    ContextAudioProcessor& processor;
    IconButton rewind { IconButton::Icon::Rewind, "Back to bar 1" };
    IconButton play { IconButton::Icon::Play, "Play / stop (both lanes stay in sync)" };
    IconButton loop { IconButton::Icon::Loop, "Loop" };
    SegmentedControl mode;
    IconButton favorite { IconButton::Icon::Star, "Favorite (keeps it in History and protects it from cleanup)" };
    juce::TextButton regenerate { "New take" }, variation { "Variation" };
    IconButton saveButton { IconButton::Icon::Save, "Save the processed sound as..." };
    IconButton revealButton { IconButton::Icon::Folder, "Show the processed file in its folder" };
    IconButton copyButton { IconButton::Icon::Copy, "Copy the processed file's path" };
    DragToDawPill drag;
    juce::Rectangle<int> positionArea;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String positionText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportBar)
};

/** Transient message near the bottom of the workspace. */
class Toast : public juce::Component, private juce::Timer
{
public:
    Toast();
    void show (const juce::String& text, bool error, std::function<void()> details = {});
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    int preferredWidth() const;

    void dismiss();

private:
    void timerCallback() override;
    juce::String message;
    bool isError = false;
    motion::Value shown { 0.0f };
    std::function<void()> onDetails;
    juce::TextButton detailsButton { "What happened" };
    IconButton close { IconButton::Icon::Close, "Dismiss" };
};
} // namespace context::ui
