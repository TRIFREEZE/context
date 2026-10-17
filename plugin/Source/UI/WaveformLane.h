#pragma once

#include "Theme.h"
#include "Motion.h"
#include "Audio/AudioAsset.h"
#include "Audio/MusicalTime.h"

namespace context::ui
{
/** The musical timeline shared by both lanes and the ruler: one sample-accurate
    mapping between timeline samples, bars and pixels. */
struct TimelineView
{
    juce::int64 lengthSamples = 0;     // timeline length (longest lane, or the selected length)
    double sampleRate = 48000.0;
    musical::Tempo tempo;

    double samplesPerBar() const       { return musical::samplesPerBar (tempo, sampleRate); }
    double bars() const                { const double s = samplesPerBar(); return s > 0.0 ? (double) lengthSamples / s : 0.0; }
    float xForSample (double sample, juce::Rectangle<float> area) const
    {
        return lengthSamples > 0 ? area.getX() + (float) (sample / (double) lengthSamples) * area.getWidth() : area.getX();
    }
    bool operator== (const TimelineView& o) const { return lengthSamples == o.lengthSamples && sampleRate == o.sampleRate && tempo == o.tempo; }
};

/** Bar numbers above the lanes. */
class TimelineRuler : public juce::Component
{
public:
    void setTimeline (const TimelineView&);
    void setPlayhead (juce::int64 samples, bool visible);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent& e) override { mouseDrag (e); }
    void mouseDrag (const juce::MouseEvent&) override;

    std::function<void (juce::int64 sample)> onSeek;   // click / scrub on the ruler

private:
    TimelineView view;
    juce::int64 playhead = 0;
    bool showPlayhead = false;
};

/** One waveform lane: cached waveform image on the shared bar grid, playhead, and the
    empty / loading / missing / drop-target states. The output lane is also the drag
    source for drag-to-DAW and accepts audio files dropped from Explorer/Finder/DAWs. */
class WaveformLane : public juce::Component,
                     public juce::FileDragAndDropTarget,
                     public juce::SettableTooltipClient
{
public:
    explicit WaveformLane (juce::Colour waveColour, bool acceptsDrops);

    void setTimeline (const TimelineView&);
    void setPeaks (WaveformPeaks::Ptr, juce::int64 laneLength);
    void setPlayhead (juce::int64 samples, bool visible);
    void setEmptyText (const juce::String& primary, const juce::String& secondary = {});
    void setBusy (bool busy, const juce::String& text = {}, double progress = -1.0);
    void setMissing (const juce::String& message);
    void setCaptureProgress (double progress, bool active);   // context lane while recording
    /** Sound lane while a new take is being generated: a living waveform being "written" across
        the lane (the old take dims underneath), with the generator's status. */
    void setGenerating (bool active, const juce::String& status = {}, double progress = -1.0);
    /** The next waveform is a new sound (a new take, a fresh capture): it draws itself in from
        left to right instead of cross-fading. */
    void revealNext() { revealPending = true; }

    /** Return the file to drag to the DAW (rendered on demand); empty = no drag. */
    std::function<juce::File()> onDragOut;
    std::function<void (const juce::File&)> onFileDropped;
    std::function<void (const juce::MouseEvent&)> onRightClick;
    std::function<void()> onClick;

    bool hasAudio() const { return peaks != nullptr; }

    void paint (juce::Graphics&) override;
    void resized() override { invalidate(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int, int) override;

    static bool looksLikeAudio (const juce::String& path);

private:
    void invalidate() { cacheValid = false; repaint(); }
    void renderCache();
    juce::Rectangle<float> waveArea() const;

    juce::Colour colour;
    bool dropTarget;
    TimelineView view;
    WaveformPeaks::Ptr peaks;
    juce::int64 laneLength = 0;
    juce::int64 playhead = 0;
    bool showPlayhead = false;
    juce::String emptyPrimary, emptySecondary, busyText, missingText;
    bool busy = false;
    double busyProgress = -1.0;
    double captureProgress = 0.0;
    bool capturing = false;
    bool dragHover = false, dragging = false;

    void paintGenerating (juce::Graphics&, juce::Rectangle<float> area, bool empty);
    bool generating = false;
    juce::String generatingStatus;
    double generatingProgress = -1.0;
    motion::Value generatingFade { 0.0f };
    double generatingStartMs = 0.0;

    juce::Image cache, previous;   // previous: the old waveform, cross-faded out on a change
    bool cacheValid = false;
    motion::Value swap { 1.0f };
    motion::Value reveal { 1.0f };      // 0..1 wipe position of a new sound
    bool revealPending = false;
};
} // namespace context::ui
