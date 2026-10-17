#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

namespace context::ui
{
/** Shows a generated file's waveform (JUCE AudioThumbnail, loaded on its own
    background thread) and acts as the drag source for drag-to-DAW. */
class WaveformView : public juce::Component, public juce::SettableTooltipClient, private juce::ChangeListener
{
public:
    WaveformView();
    ~WaveformView() override;

    void setFile (const juce::File&);
    juce::File getFile() const { return file; }
    void setPlayhead (double seconds, bool visible);

    /** Called right before an external drag starts (e.g. to pin the file). */
    std::function<void()> onDragStarted;
    /** Click on the waveform. */
    std::function<void()> onClicked;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

    juce::AudioFormatManager formats;
    juce::AudioThumbnailCache cache { 8 };
    juce::AudioThumbnail thumbnail { 512, formats, cache };
    juce::File file;
    double playhead = 0.0;
    bool showPlayhead = false;
    bool dragging = false;
};
} // namespace context::ui
