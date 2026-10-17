#pragma once

#include "Controls.h"
#include "WaveformLane.h"
#include "Session/SessionController.h"

namespace context::ui
{
/** Editing on the sound lane, drawn over its waveform.

      drag on the waveform         select a range (snaps to the grid; Alt = free)
      drag a clip's name strip     move the clip
      drag a clip edge             trim it (or stretch it in Stretch mode; Alt = the other one)
      drag a clip's corner grip    stretch it, from either end
      double-click a clip          select the clip
      click                        move the playhead
      right-click                  everything else (split, fill, takes, copy/paste...)

    Every gesture is ONE session edit on mouse-up, so it is one undo step. Positions are in
    beats; the overlay only converts to pixels. */
class ArrangementOverlay : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit ArrangementOverlay (SessionController&);

    void setTimeline (const TimelineView&);
    void update();                                  // pull the edit view / takes from the session
    void setPlayheadBeat (double beat, bool visible);

    std::optional<std::pair<double, double>> selection() const { return sel; }
    void setSelection (std::optional<std::pair<double, double>>);
    void selectAll();
    double playheadBeat() const { return playhead; }
    double snapBeat (double beat, bool free) const;
    /** Edge drags stretch (true) or trim (false). The corner grips always stretch. */
    void setStretchEdges (bool b) { stretchEdges = b; }
    bool getStretchEdges() const { return stretchEdges; }

    std::function<void (double beat)> onSeek;
    std::function<void (double from, double to)> onFillWithNewTake;
    std::function<juce::File()> onDragOut;
    std::function<void()> onSelectionChanged;
    std::function<void (juce::Point<int>)> onContextMenu;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    juce::MouseCursor getMouseCursor() override;

    static constexpr int stripHeight = 17;

private:
    enum class Drag { None, Select, Move, TrimStart, TrimEnd, Stretch, StretchStart, Out };
    struct Hit { int clip = -1; Drag edge = Drag::None; bool strip = false; bool dragHandle = false; bool grip = false; };
    static bool isStretch (Drag d) { return d == Drag::Stretch || d == Drag::StretchStart; }
    Drag resolveEdge (const Hit&, bool alt) const;
    juce::Rectangle<float> gripArea (int clip, bool atStart) const;

    Hit hitTest (juce::Point<float>) const;
    float xForBeat (double beat) const;
    double beatForX (float x) const;
    juce::Rectangle<float> dragHandleArea() const;
    bool hasAudio() const { return ! view.clips.empty() && timeline.lengthSamples > 0; }

    SessionController& session;
    TimelineView timeline;
    SessionController::EditView view;
    std::map<juce::String, int> takeNumbers;
    std::optional<std::pair<double, double>> sel;
    double playhead = 0.0;
    bool showPlayhead = false;

    Drag drag = Drag::None;
    int dragClip = -1;
    double dragAnchor = 0.0, dragOffset = 0.0;
    edit::Clip ghost;                               // preview while moving / trimming / stretching
    bool ghostActive = false, dragMoved = false;
    Hit hover;

    motion::Value selAlpha { 0.0f };
    bool stretchEdges = false;
};
} // namespace context::ui
