#include "WaveformLane.h"
#include "Controls.h"
#include "Motion.h"

namespace context::ui
{
namespace
{
    void drawGrid (juce::Graphics& g, const TimelineView& v, juce::Rectangle<float> area, bool beats)
    {
        const double spb = v.samplesPerBar();
        if (spb <= 0.0 || v.lengthSamples <= 0)
            return;
        const int bars = (int) std::ceil (v.bars() - 1.0e-6);
        const int beatsPerBar = juce::jmax (1, v.tempo.signature.numerator);
        const double pxPerBar = area.getWidth() * spb / (double) v.lengthSamples;
        for (int b = 0; b <= bars; ++b)
        {
            const float x = std::round (v.xForSample (b * spb, area));
            if (x > area.getRight() + 0.5f)
                break;
            g.setColour (b == 0 ? colours::grid : colours::line);
            g.fillRect (x, area.getY(), 1.0f, area.getHeight());
            if (beats && pxPerBar / beatsPerBar > 9.0)
                for (int k = 1; k < beatsPerBar; ++k)
                {
                    const float bx = std::round (v.xForSample ((b + (double) k / beatsPerBar) * spb, area));
                    if (bx >= area.getRight())
                        break;
                    g.setColour (colours::grid);
                    g.fillRect (bx, area.getBottom() - 4.0f, 1.0f, 4.0f);
                    g.fillRect (bx, area.getY(), 1.0f, 3.0f);
                }
        }
    }
}

// ===========================================================================
// Ruler
// ===========================================================================
void TimelineRuler::setTimeline (const TimelineView& v)
{
    if (v == view) return;
    view = v;
    repaint();
}

void TimelineRuler::setPlayhead (juce::int64 samples, bool visible)
{
    if (samples == playhead && visible == showPlayhead) return;
    playhead = samples;
    showPlayhead = visible;
    repaint();
}

void TimelineRuler::mouseDrag (const juce::MouseEvent& e)
{
    if (onSeek && view.lengthSamples > 0 && getWidth() > 0)
        onSeek ((juce::int64) juce::jlimit (0.0, (double) view.lengthSamples - 1.0, (double) e.position.x / getWidth() * (double) view.lengthSamples));
}

void TimelineRuler::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    const double spb = view.samplesPerBar();
    if (spb <= 0.0 || view.lengthSamples <= 0)
        return;
    const int bars = (int) std::ceil (view.bars() - 1.0e-6);
    const double pxPerBar = area.getWidth() * spb / (double) view.lengthSamples;
    const int every = pxPerBar < 22.0 ? 4 : pxPerBar < 36.0 ? 2 : 1;
    g.setFont (numberFont (type::meta, Weight::Medium));
    for (int b = 0; b < bars; ++b)
    {
        const float x = std::round (view.xForSample (b * spb, area));
        g.setColour (colours::border);
        g.fillRect (x, area.getBottom() - 5.0f, 1.0f, 5.0f);
        if (b % every == 0)
        {
            g.setColour (colours::faint);
            g.drawText (juce::String (b + 1), juce::Rectangle<float> (x + 3.0f, area.getY(), 30.0f, area.getHeight() - 2.0f), juce::Justification::centredLeft, false);
        }
    }
    if (showPlayhead)
    {
        const float x = view.xForSample ((double) playhead, area);
        juce::Path tri;
        tri.addTriangle (x - 4.0f, area.getBottom() - 6.0f, x + 4.0f, area.getBottom() - 6.0f, x, area.getBottom());
        g.setColour (colours::text);
        g.fillPath (tri);
    }
}

// ===========================================================================
// Lane
// ===========================================================================
WaveformLane::WaveformLane (juce::Colour c, bool acceptsDrops) : colour (c), dropTarget (acceptsDrops)
{
    swap.onChange = [this] { repaint(); };
}

bool WaveformLane::looksLikeAudio (const juce::String& path)
{
    const auto ext = juce::File (path).getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac" || ext == ".ogg" || ext == ".mp3" || ext == ".m4a" || ext == ".wma";
}

void WaveformLane::setTimeline (const TimelineView& v)
{
    if (v == view) return;
    view = v;
    invalidate();
}

void WaveformLane::setPeaks (WaveformPeaks::Ptr p, juce::int64 length)
{
    if (p == peaks && length == laneLength) return;
    // A new waveform (re-fit, edit, new take) cross-fades over the old one, so the change is visible.
    if (cacheValid && cache.isValid() && peaks != nullptr && p != nullptr && motion::enabled())
    {
        previous = cache;
        swap.snap (0.0f);
        swap.set (1.0f, motion::duration::panel, motion::Ease::OutCubic);
    }
    else
    {
        previous = {};
        swap.snap (1.0f);
    }
    peaks = std::move (p);
    laneLength = length;
    invalidate();
}

void WaveformLane::setPlayhead (juce::int64 samples, bool visible)
{
    if (samples == playhead && visible == showPlayhead) return;
    const auto area = waveArea();
    const float oldX = view.xForSample ((double) playhead, area), newX = view.xForSample ((double) samples, area);
    playhead = samples;
    const bool wasVisible = showPlayhead;
    showPlayhead = visible;
    if (wasVisible) repaint (juce::Rectangle<float> (oldX - 2.0f, 0.0f, 4.0f, (float) getHeight()).toNearestInt());
    if (visible)    repaint (juce::Rectangle<float> (newX - 2.0f, 0.0f, 4.0f, (float) getHeight()).toNearestInt());
}

void WaveformLane::setEmptyText (const juce::String& primary, const juce::String& secondary)
{
    if (primary == emptyPrimary && secondary == emptySecondary) return;
    emptyPrimary = primary;
    emptySecondary = secondary;
    repaint();
}

void WaveformLane::setBusy (bool b, const juce::String& text, double progress)
{
    if (b == busy && text == busyText && std::abs (progress - busyProgress) < 0.002) return;
    busy = b;
    busyText = text;
    busyProgress = progress;
    repaint();
}

void WaveformLane::setGenerating (bool active, const juce::String& status, double progress)
{
    if (active != generating)
    {
        generating = active;
        if (active)
            generatingStartMs = motion::nowMs();
        generatingFade.onChange = [this] { repaint(); };
        generatingFade.set (active ? 1.0f : 0.0f, active ? motion::duration::panel : motion::duration::panel * 1.5f, motion::Ease::OutCubic);
    }
    if (status != generatingStatus || std::abs (progress - generatingProgress) > 0.002)
    {
        generatingStatus = status;
        generatingProgress = progress;
        repaint();
    }
}

void WaveformLane::paintGenerating (juce::Graphics& g, juce::Rectangle<float> area, bool empty)
{
    const float fade = generatingFade.get();
    if (fade <= 0.001f)
        return;
    const bool moving = motion::enabled();
    const double t = moving ? (motion::nowMs() - generatingStartMs) / 1000.0 : 0.0;

    // The old take steps back.
    if (! empty)
    {
        g.setColour (colours::well.withAlpha (0.86f * fade));
        g.fillRect (area);
    }

    // Where the "writing" head is: the real progress when the generator reports it, otherwise a
    // calm sweep that eases across and starts again.
    double head;
    if (generatingProgress >= 0.0)
        head = juce::jlimit (0.0, 1.0, generatingProgress);
    else
    {
        const double cycle = std::fmod (t / 3.2, 1.0);
        head = 0.5 - 0.5 * std::cos (cycle * juce::MathConstants<double>::pi);   // ease in-out
    }

    // A field of thin bars whose heights drift like a waveform being imagined.
    const float barW = 2.0f, gap = 3.0f;
    const int n = juce::jmax (8, (int) (area.getWidth() / (barW + gap)));
    const float cy = area.getCentreY(), halfH = area.getHeight() * 0.42f;
    const auto accent = colours::waveform;
    for (int i = 0; i < n; ++i)
    {
        const double u = (i + 0.5) / n;
        const double edge = juce::jlimit (0.0, 1.0, juce::jmin (u, 1.0 - u) / 0.06);
        const double env = edge * edge * (3.0 - 2.0 * edge);                         // smoothstep at the lane ends
        const double wave = 0.5 + 0.5 * std::sin (juce::MathConstants<double>::twoPi * (u * 3.1 + t * 0.21))
                                       * std::sin (juce::MathConstants<double>::twoPi * (u * 7.3 - t * 0.37));
        const double grain = 0.5 + 0.5 * std::sin (i * 2.399 + t * 1.7);           // per-bar shimmer
        const double amp = env * (0.16 + 0.62 * wave + 0.22 * grain);
        const double d = u - head;                                                   // <0: already written
        float alpha;
        if (d <= 0.0)
            alpha = (float) (0.20 + 0.38 * std::exp (d * 5.0));                        // trail that settles
        else
            alpha = (float) (0.07 * std::exp (-d * 2.0));                              // not yet written
        const float glow = (float) std::exp (-(d * d) / (2.0 * 0.018 * 0.018));        // bright at the head
        alpha = juce::jlimit (0.0f, 1.0f, alpha + 0.6f * glow) * fade;
        const float h = juce::jmax (1.5f, (float) amp * halfH * (1.0f + 0.25f * glow));
        const float x = area.getX() + (float) u * area.getWidth() - barW * 0.5f;
        g.setColour (accent.withAlpha (alpha));
        g.fillRoundedRectangle (x, cy - h, barW, h * 2.0f, 1.0f);
    }

    // The head itself: a fine line with a soft halo.
    const float hx = area.getX() + (float) head * area.getWidth();
    juce::ColourGradient halo (accent.withAlpha (0.0f), hx - 26.0f, cy, accent.withAlpha (0.0f), hx + 26.0f, cy, false);
    halo.addColour (0.5, accent.withAlpha (0.16f * fade));
    g.setGradientFill (halo);
    g.fillRect (juce::Rectangle<float> (hx - 26.0f, area.getY(), 52.0f, area.getHeight()));
    g.setColour (accent.withAlpha (0.85f * fade));
    g.fillRect (hx - 0.75f, area.getY() + 6.0f, 1.5f, area.getHeight() - 12.0f);

    // Status, on a quiet pill.
    const auto f1 = font (type::body, Weight::SemiBold), f2 = font (type::label);
    const juce::String title = "Generating";
    const auto status = generatingStatus;
    const float w1 = (float) juce::GlyphArrangement::getStringWidthInt (f1, title);
    const float w2 = status.isNotEmpty() ? (float) juce::GlyphArrangement::getStringWidthInt (f2, status) : 0.0f;
    // 14 pad + 6 dot + 6 gap + title + (10 gap + status) + 16 pad
    const float pillW = juce::jmin (area.getWidth() - 24.0f, 14.0f + 12.0f + w1 + 2.0f + (w2 > 0.0f ? 10.0f + w2 + 2.0f : 0.0f) + 16.0f);
    auto pill = juce::Rectangle<float> (pillW, 26.0f).withCentre (area.getCentre());
    g.setColour (colours::panel.withAlpha (0.92f * fade));
    g.fillRoundedRectangle (pill, 13.0f);
    g.setColour (colours::border.withAlpha (fade));
    g.drawRoundedRectangle (pill.reduced (0.5f), 13.0f, 1.0f);
    auto inner = pill.withTrimmedLeft (14.0f).withTrimmedRight (14.0f);
    // A breathing dot: the generator is alive.
    const float pulse = moving ? 0.55f + 0.45f * (float) std::sin (t * 3.4) : 1.0f;
    g.setColour (accent.withAlpha (fade * (0.45f + 0.55f * pulse)));
    g.fillEllipse (inner.getX(), pill.getCentreY() - 3.0f, 6.0f, 6.0f);
    inner.removeFromLeft (12.0f);
    g.setColour (colours::text.withAlpha (fade));
    g.setFont (f1);
    g.drawText (title, inner.removeFromLeft (w1 + 2.0f), juce::Justification::centredLeft, false);
    if (w2 > 0.0f)
    {
        inner.removeFromLeft (10.0f);
        g.setColour (colours::faint.withAlpha (fade));
        g.setFont (f2);
        g.drawText (status, inner, juce::Justification::centredLeft, true);
    }

    if (moving && (generating || fade > 0.001f))
        motion::Clock::instance().repaintUntil (*this, motion::nowMs() + 100.0);
}

void WaveformLane::setMissing (const juce::String& message)
{
    if (message == missingText) return;
    missingText = message;
    repaint();
}

void WaveformLane::setCaptureProgress (double progress, bool active)
{
    if (std::abs (progress - captureProgress) < 0.001 && active == capturing) return;
    captureProgress = progress;
    capturing = active;
    repaint();
}

juce::Rectangle<float> WaveformLane::waveArea() const
{
    return getLocalBounds().toFloat().reduced (0.0f, 1.0f);
}

void WaveformLane::renderCache()
{
    const float scale = 2.0f;   // render at 2x for crisp lines on HiDPI
    const int w = juce::jmax (1, getWidth()), h = juce::jmax (1, getHeight());
    cache = juce::Image (juce::Image::ARGB, (int) (w * scale), (int) (h * scale), true);
    juce::Graphics g (cache);
    g.addTransform (juce::AffineTransform::scale (scale));
    const auto area = waveArea();

    drawGrid (g, view, area, true);

    if (peaks != nullptr && laneLength > 0 && view.lengthSamples > 0)
    {
        const float mid = area.getCentreY(), half = area.getHeight() * 0.46f;
        const int px = (int) std::ceil (area.getWidth() * scale);
        const double samplesPerPx = (double) view.lengthSamples / (double) px;
        // Peaks are stored at the asset's own sample rate; the timeline runs at the host rate.
        const double toPeak = peaks->sampleRate() > 0.0 && view.sampleRate > 0.0 ? peaks->sampleRate() / view.sampleRate : 1.0;
        // Lanes shorter than the timeline repeat during playback: draw the repeats dimmer.
        for (int pass = 0; pass < 2; ++pass)
        {
            juce::Path top;
            bool open = false;
            juce::Array<juce::Point<float>> bottom;
            for (int i = 0; i < px; ++i)
            {
                const double s0 = i * samplesPerPx, s1 = (i + 1) * samplesPerPx;
                const bool repeat = (juce::int64) s0 >= laneLength;
                if (repeat != (pass == 1))
                {
                    if (open)
                    {
                        for (int k = bottom.size(); --k >= 0;) top.lineTo (bottom[k]);
                        top.closeSubPath();
                        bottom.clearQuick();
                        open = false;
                    }
                    continue;
                }
                const auto a = (juce::int64) s0 % laneLength;
                const auto b = juce::jmin (laneLength, a + juce::jmax ((juce::int64) 1, (juce::int64) std::ceil (s1 - s0)));
                const auto r = peaks->range ((juce::int64) (a * toPeak), juce::jmax ((juce::int64) (a * toPeak) + 1, (juce::int64) std::ceil (b * toPeak)));
                const float x = area.getX() + (float) i / scale;
                const float y0 = mid - juce::jlimit (0.0f, 1.0f, r.second) * half - 0.4f;
                const float y1 = mid - juce::jlimit (-1.0f, 0.0f, r.first) * half + 0.4f;
                if (! open) { top.startNewSubPath (x, y0); open = true; }
                else top.lineTo (x, y0);
                bottom.add ({ x, y1 });
            }
            if (open)
            {
                for (int k = bottom.size(); --k >= 0;) top.lineTo (bottom[k]);
                top.closeSubPath();
            }
            g.setColour (pass == 0 ? colour.withAlpha (0.92f) : colour.withAlpha (0.32f));
            g.fillPath (top);
        }
        // Lane end marker when the lane is shorter than the timeline.
        if (laneLength < view.lengthSamples)
        {
            const float x = std::round (view.xForSample ((double) laneLength, area));
            g.setColour (colour.withAlpha (0.5f));
            g.fillRect (x, area.getY(), 1.0f, area.getHeight());
        }
        g.setColour (colour.withAlpha (0.18f));
        g.fillRect (area.getX(), mid, area.getWidth(), 0.5f);
    }
    cacheValid = true;
}

void WaveformLane::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (colours::well);
    g.fillRoundedRectangle (r, radius::control);

    if (! cacheValid || cache.getWidth() != getWidth() * 2)
        renderCache();
    const float s = swap.get();
    if (previous.isValid() && s < 1.0f && previous.getWidth() == cache.getWidth())
    {
        g.setOpacity (1.0f - s);
        g.drawImage (previous, r, juce::RectanglePlacement::stretchToFit);
        g.setOpacity (s);
        g.drawImage (cache, r, juce::RectanglePlacement::stretchToFit);
        g.setOpacity (1.0f);
    }
    else
    {
        if (s >= 1.0f) previous = {};
        g.drawImage (cache, r, juce::RectanglePlacement::stretchToFit);
    }

    const auto area = waveArea();
    const bool empty = peaks == nullptr;
    auto centreText = [&] (const juce::String& a, const juce::String& b, juce::Colour ca)
    {
        auto t = getLocalBounds().reduced (12, 0);
        const int lines = b.isNotEmpty() ? 2 : 1;
        auto block = t.withSizeKeepingCentre (t.getWidth(), lines * 18);
        g.setColour (ca);
        g.setFont (font (type::body + 1.0f, Weight::Medium));
        g.drawText (a, block.removeFromTop (18), juce::Justification::centred, true);
        if (b.isNotEmpty())
        {
            g.setColour (colours::faint);
            g.setFont (font (type::body));
            g.drawText (b, block, juce::Justification::centred, true);
        }
    };

    if (capturing)
    {
        // Recording: the amber lamp fills the bars as they are recorded.
        const float x = area.getX() + area.getWidth() * (float) captureProgress;
        g.setColour (colours::lamp.withAlpha (0.10f));
        g.fillRect (area.withRight (x));
        g.setColour (colours::lamp);
        g.fillRect (x - 1.0f, area.getY(), 2.0f, area.getHeight());
    }
    else if (generatingFade.get() > 0.001f && ! busy)
    {
        paintGenerating (g, area, empty);
    }
    else if (busy)
    {
        g.setColour (colours::background.withAlpha (empty ? 0.0f : 0.55f));
        g.fillRoundedRectangle (r, radius::control);
        drawProgress (g, juce::Rectangle<float> (area.getCentreX() - 60.0f, area.getCentreY() + 12.0f, 120.0f, 3.0f), busyProgress);
        g.setColour (colours::dim);
        g.setFont (font (type::body, Weight::Medium));
        g.drawText (busyText, getLocalBounds().withTrimmedBottom (14), juce::Justification::centred, true);
        if (motion::enabled() && busyProgress < 0.0)
            motion::Clock::instance().repaintUntil (*this, motion::nowMs() + 100.0);   // indeterminate bar keeps moving
    }
    else if (missingText.isNotEmpty() && empty)
    {
        centreText (missingText, {}, colours::warn);
    }
    else if (empty && emptyPrimary.isNotEmpty())
    {
        centreText (emptyPrimary, emptySecondary, colours::dim);
    }

    if (showPlayhead && view.lengthSamples > 0)
    {
        const float x = view.xForSample ((double) playhead, area);
        g.setColour (colours::text.withAlpha (0.9f));
        g.fillRect (x - 0.5f, area.getY(), 1.5f, area.getHeight());
    }

    if (dragHover)
    {
        g.setColour (colours::accent.withAlpha (0.10f));
        g.fillRoundedRectangle (r, radius::control);
        g.setColour (colours::accent);
        g.drawRoundedRectangle (r.reduced (1.0f), radius::control, 1.5f);
        g.setColour (colours::panel.withAlpha (0.9f));
        auto pill = getLocalBounds().withSizeKeepingCentre (230, 28).toFloat();
        g.fillRoundedRectangle (pill, radius::control);
        g.setColour (colours::accent);
        g.setFont (font (type::body, Weight::SemiBold));
        g.drawText (empty ? "Drop to load this sample" : "Drop to replace the sound", pill, juce::Justification::centred);
    }
}

void WaveformLane::mouseDown (const juce::MouseEvent& e)
{
    dragging = false;
    if (e.mods.isPopupMenu() && onRightClick)
        onRightClick (e);
}

void WaveformLane::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || e.mods.isPopupMenu() || e.getDistanceFromDragStart() < 6 || ! onDragOut || peaks == nullptr)
        return;
    dragging = true;
    const auto file = onDragOut();
    if (! file.existsAsFile())
        return;
    // OS-level file drag (OLE on Windows): FL Studio, Ableton, REAPER, Studio One and Cubase accept WAV drops.
    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this);
}

void WaveformLane::mouseUp (const juce::MouseEvent& e)
{
    if (! dragging && ! e.mods.isPopupMenu() && e.getDistanceFromDragStart() < 6 && onClick)
        onClick();
    dragging = false;
}

bool WaveformLane::isInterestedInFileDrag (const juce::StringArray& files)
{
    return dropTarget && files.size() == 1 && looksLikeAudio (files[0]);
}

void WaveformLane::fileDragEnter (const juce::StringArray&, int, int)
{
    dragHover = true;
    repaint();
}

void WaveformLane::fileDragExit (const juce::StringArray&)
{
    dragHover = false;
    repaint();
}

void WaveformLane::filesDropped (const juce::StringArray& files, int, int)
{
    dragHover = false;
    repaint();
    if (! files.isEmpty() && onFileDropped)
        onFileDropped (juce::File (files[0]));
}
} // namespace context::ui
