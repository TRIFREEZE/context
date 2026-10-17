#pragma once

#include "Controls.h"
#include "WaveformLane.h"

namespace context { class ContextAudioProcessor; }

namespace context::ui
{
/** TRACK CONTEXT above AI / SAMPLE on one musical timeline, plus the fit controls. */
class LanesPanel : public juce::Component
{
public:
    explicit LanesPanel (ContextAudioProcessor&);
    ~LanesPanel() override;

    void refresh();
    void tick();
    void openSampleChooser();

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int headerWidth = layout::laneHeader;

private:
    TimelineView computeTimeline() const;
    void showContextMenu();
    void showOutputMenu();
    void layoutLaneHeader (juce::Rectangle<int> area, bool context);
    juce::String outputDetail() const;
    juce::String fitSummary() const;

    ContextAudioProcessor& processor;

    TimelineRuler ruler;
    WaveformLane contextLane { colours::contextWave, false };
    WaveformLane outputLane { colours::waveform, true };

    juce::TextButton captureButton { "Capture" }, loadButton { "Load sample" };
    juce::TextButton retargetButton, recaptureButton { "Capture again" };
    LaneToggle ctxMute { "M", colours::warn }, ctxSolo { "S", colours::accent }, outMute { "M", colours::warn }, outSolo { "S", colours::accent };
    juce::Slider ctxGain, outGain;
    IconButton ctxMenu { IconButton::Icon::More, "Context options" }, outMenu { IconButton::Icon::More, "Sample options" };
    juce::Label manualTempo;

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
