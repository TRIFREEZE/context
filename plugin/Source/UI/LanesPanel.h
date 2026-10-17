#pragma once

#include "Controls.h"
#include "WaveformLane.h"
#include "ArrangementOverlay.h"
#include "SelectionBar.h"
#include "TempoKeySheets.h"

namespace context { class ContextAudioProcessor; }

namespace context::ui
{
/** "Your track" (the captured context) above "Sound" (generated / imported, editable) on
    one musical timeline, plus the fit controls. */
class LanesPanel : public juce::Component
{
public:
    explicit LanesPanel (ContextAudioProcessor&);
    ~LanesPanel() override;

    void refresh();
    void tick();
    void openSampleChooser();
    /** Editing keys (S, Delete, F, Ctrl+C/X/V/D/A, G, Esc). True if used. */
    bool handleKey (const juce::KeyPress&);
    ArrangementOverlay& arrangement() { return overlay; }
    /** The right-click menu of the sound lane (public for the screenshot tests). */
    void showEditMenu (juce::Point<int> inOverlay);

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int headerWidth = layout::laneHeader;

private:
    TimelineView computeTimeline() const;
    void showContextMenu();
    void showOutputMenu();
    void showTakesMenu();
    void showFillFromMenu (juce::Component* target);
    void addOutputItems (juce::PopupMenu&, int base);
    void handleOutputItem (int r);
    double playheadBeat() const;
    void splitAtSelectionOrPlayhead();
    void deleteSelection();
    void fillWithNewTake();
    void updateSelectionBar();
    juce::String rangeText (double from, double to) const;
    void layoutLaneHeader (juce::Rectangle<int> area, bool context);
    juce::String outputDetail() const;
    juce::String fitSummary() const;
    juce::String bpmLabel() const;

    ContextAudioProcessor& processor;

    TimelineRuler ruler;
    WaveformLane contextLane { colours::contextWave, false };
    WaveformLane outputLane { colours::waveform, true };
    ArrangementOverlay overlay;
    SelectionBar selectionBar;
    juce::ComboBox snap;
    SegmentedControl edgeMode;            // what dragging a clip edge does: Trim | Stretch
    void setStretchEdges (bool);
    juce::TextButton takesButton;

    juce::TextButton captureButton { "Capture" }, loadButton { "Load sample" };
    juce::TextButton retargetButton, recaptureButton { "Capture again" };
    LaneToggle ctxMute { "M", colours::warn }, ctxSolo { "S", colours::accent }, outMute { "M", colours::warn }, outSolo { "S", colours::accent };
    juce::Slider ctxGain, outGain;
    IconButton ctxMenu { IconButton::Icon::More, "Context options" }, outMenu { IconButton::Icon::More, "Sample options" };
    ValueChip bpmChip, keyChip;           // click to set the tempo / key

    SegmentedControl fitMode;
    juce::ComboBox algorithm;
    juce::Slider pitch;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> ctxHeader, outHeader, fitHeader, fitInfo, rulerArea, pitchLabel, pitchValue;
    bool tempoMismatch = false;
    bool wasRecording = false;
    int lastBarsShown = -1;
    TimelineView timeline;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LanesPanel)
};
} // namespace context::ui
