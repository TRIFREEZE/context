#include "WaveformView.h"
#include "Theme.h"

namespace context::ui
{
WaveformView::WaveformView()
{
    formats.registerBasicFormats();
    thumbnail.addChangeListener (this);
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    setTooltip ("Drag this sound into your DAW");
}

WaveformView::~WaveformView()
{
    thumbnail.removeChangeListener (this);
    thumbnail.clear();
}

void WaveformView::setFile (const juce::File& f)
{
    if (f == file)
        return;
    file = f;
    if (file.existsAsFile())
        thumbnail.setSource (new juce::FileInputSource (file));   // parsed on the cache's background thread
    else
        thumbnail.clear();
    repaint();
}

void WaveformView::setPlayhead (double seconds, bool visible)
{
    if (std::abs (seconds - playhead) < 1.0e-3 && visible == showPlayhead)
        return;
    playhead = seconds;
    showPlayhead = visible;
    repaint();
}

void WaveformView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff101216));
    g.fillRoundedRectangle (r, 6.0f);

    if (! file.existsAsFile())
    {
        g.setColour (colours::faint);
        g.setFont (ContextLookAndFeel::font (14.0f));
        g.drawText (file == juce::File() ? "Your generated sound will appear here" : "File missing: " + file.getFileName(),
                    getLocalBounds(), juce::Justification::centred);
        return;
    }

    auto wave = getLocalBounds().reduced (8, 10);
    if (thumbnail.getTotalLength() > 0.0)
    {
        g.setColour (colours::waveform.withAlpha (isMouseOver() ? 0.95f : 0.8f));
        thumbnail.drawChannels (g, wave, 0.0, thumbnail.getTotalLength(), 0.95f);

        if (showPlayhead && thumbnail.getTotalLength() > 0.0)
        {
            const float x = (float) wave.getX() + (float) (playhead / thumbnail.getTotalLength()) * (float) wave.getWidth();
            g.setColour (colours::text);
            g.drawLine (x, (float) wave.getY(), x, (float) wave.getBottom(), 1.5f);
        }
    }
    else
    {
        g.setColour (colours::faint);
        g.drawText ("Loading waveform...", getLocalBounds(), juce::Justification::centred);
    }

    // Drag hint
    auto hint = getLocalBounds().removeFromRight (150).removeFromBottom (26).reduced (6, 3).toFloat();
    g.setColour (juce::Colour (0xcc0e0f12));
    g.fillRoundedRectangle (hint, 5.0f);
    g.setColour (isMouseOver() ? colours::accent : colours::dim);
    g.setFont (ContextLookAndFeel::font (11.5f, true).withExtraKerningFactor (0.08f));
    g.drawText (glyph ("\xe2\x87\xa2  DRAG TO DAW"), hint, juce::Justification::centred);
}

void WaveformView::mouseDown (const juce::MouseEvent&)
{
    dragging = false;
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || e.getDistanceFromDragStart() < 6 || ! file.existsAsFile())
        return;
    dragging = true;
    if (onDragStarted)
        onDragStarted();
    // OS-level file drag (OLE on Windows): FL Studio, Ableton, REAPER, Studio One and Cubase accept WAV drops.
    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this);
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    if (! dragging && e.getDistanceFromDragStart() < 6 && onClicked)
        onClicked();
    dragging = false;
}
} // namespace context::ui
