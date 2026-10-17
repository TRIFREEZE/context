#include "ArrangementOverlay.h"

namespace context::ui
{
namespace
{
    constexpr float edgeGrab = 5.0f;

    juce::String ratioText (double stretch)
    {
        // Show how much longer/shorter the clip now plays (1/stretch).
        return glyph ("\xc3\x97") + juce::String (1.0 / stretch, 2);
    }
}

ArrangementOverlay::ArrangementOverlay (SessionController& s) : session (s)
{
    setInterceptsMouseClicks (true, false);
    selAlpha.onChange = [this] { repaint(); };
}

void ArrangementOverlay::setTimeline (const TimelineView& v)
{
    if (v == timeline) return;
    timeline = v;
    repaint();
}

void ArrangementOverlay::update()
{
    view = session.editView();
    takeNumbers.clear();
    for (const auto& t : session.takes())
        takeNumbers[t.id] = t.number;
    if (sel && (sel->first >= view.totalBeats || view.totalBeats <= 0.0))
        setSelection (std::nullopt);
    else if (sel)
        sel->second = juce::jmin (sel->second, view.totalBeats);
    repaint();
}

void ArrangementOverlay::setPlayheadBeat (double beat, bool visible)
{
    playhead = beat;
    showPlayhead = visible;
}

void ArrangementOverlay::setSelection (std::optional<std::pair<double, double>> s)
{
    if (s && s->second - s->first < edit::minClipBeats)
        s.reset();
    const bool had = sel.has_value();
    sel = s;
    if (sel.has_value() != had)
        selAlpha.set (sel ? 1.0f : 0.0f, sel ? motion::duration::hover : motion::duration::fade);
    repaint();
    if (onSelectionChanged)
        onSelectionChanged();
}

void ArrangementOverlay::selectAll()
{
    if (view.totalBeats > 0.0)
        setSelection (std::make_pair (0.0, view.totalBeats));
}

double ArrangementOverlay::snapBeat (double beat, bool free) const
{
    const double b = free ? beat : edit::snap (beat, view.gridBeats);
    return juce::jlimit (0.0, juce::jmax (0.0, view.totalBeats), b);
}

float ArrangementOverlay::xForBeat (double beat) const
{
    const double sample = beat * 60.0 / timeline.tempo.bpm * timeline.sampleRate;
    return timeline.xForSample (sample, getLocalBounds().toFloat());
}

double ArrangementOverlay::beatForX (float x) const
{
    if (timeline.lengthSamples <= 0 || getWidth() <= 0)
        return 0.0;
    const double sample = (double) (x / (float) getWidth()) * (double) timeline.lengthSamples;
    return sample * timeline.tempo.bpm / (60.0 * timeline.sampleRate);
}

juce::Rectangle<float> ArrangementOverlay::dragHandleArea() const
{
    return getLocalBounds().toFloat().removeFromRight (104.0f).removeFromTop (24.0f).reduced (4.0f, 4.0f).translated (0.0f, view.edited ? (float) stripHeight : 0.0f);
}

ArrangementOverlay::Hit ArrangementOverlay::hitTest (juce::Point<float> p) const
{
    Hit h;
    if (! hasAudio())
        return h;
    if (onDragOut && dragHandleArea().contains (p))
    {
        h.dragHandle = true;
        return h;
    }
    for (int i = 0; i < (int) view.clips.size(); ++i)
    {
        const auto& c = view.clips[(size_t) i];
        const float x0 = xForBeat (c.start), x1 = xForBeat (c.end());
        if (p.x >= x0 - edgeGrab && p.x <= x1 + edgeGrab)
        {
            if (std::abs (p.x - x1) <= edgeGrab) { h.clip = i; h.edge = Drag::TrimEnd; }
            else if (std::abs (p.x - x0) <= edgeGrab && (c.start > 1.0e-6 || view.edited)) { h.clip = i; h.edge = Drag::TrimStart; }
            else if (p.x > x0 && p.x < x1) h.clip = i;
            if (h.clip >= 0)
            {
                h.strip = view.edited && p.y < (float) stripHeight;
                if (h.edge == Drag::None || h.edge == Drag::TrimStart || h.edge == Drag::TrimEnd)
                    return h;
            }
        }
    }
    return h;
}

juce::MouseCursor ArrangementOverlay::getMouseCursor()
{
    if (drag == Drag::Move || hover.strip || hover.dragHandle) return juce::MouseCursor::DraggingHandCursor;
    if (drag == Drag::TrimStart || drag == Drag::TrimEnd || drag == Drag::Stretch || hover.edge != Drag::None)
        return juce::MouseCursor::LeftRightResizeCursor;
    return juce::MouseCursor::NormalCursor;
}

void ArrangementOverlay::mouseMove (const juce::MouseEvent& e)
{
    const auto h = hitTest (e.position);
    if (h.clip != hover.clip || h.edge != hover.edge || h.strip != hover.strip || h.dragHandle != hover.dragHandle)
    {
        hover = h;
        setTooltip (h.dragHandle ? "Drag the processed sound into your DAW"
                    : h.edge == Drag::TrimEnd ? "Drag to trim. Alt+drag to stretch the clip."
                    : h.edge == Drag::TrimStart ? "Drag to trim the start"
                    : h.strip ? "Drag to move this clip"
                    : "Drag to select. Right-click for split, fill, takes and more.");
        updateMouseCursor();
    }
    repaint();
}

void ArrangementOverlay::mouseExit (const juce::MouseEvent&)
{
    hover = {};
    repaint();
}

void ArrangementOverlay::mouseDown (const juce::MouseEvent& e)
{
    dragMoved = false;
    ghostActive = false;
    if (! hasAudio())
        return;
    if (e.mods.isPopupMenu())
    {
        // Right-click inside the selection keeps it; elsewhere selects nothing first.
        const double b = beatForX (e.position.x);
        if (sel && (b < sel->first || b > sel->second))
            setSelection (std::nullopt);
        if (onContextMenu)
            onContextMenu (e.getPosition());
        return;
    }
    const auto h = hitTest (e.position);
    const bool free = e.mods.isAltDown();
    if (h.dragHandle)
    {
        drag = Drag::Out;
        return;
    }
    if (h.clip >= 0 && h.edge != Drag::None)
    {
        drag = (h.edge == Drag::TrimEnd && e.mods.isAltDown()) ? Drag::Stretch : h.edge;
        dragClip = h.clip;
        ghost = view.clips[(size_t) h.clip];
        return;
    }
    if (h.clip >= 0 && h.strip)
    {
        drag = Drag::Move;
        dragClip = h.clip;
        ghost = view.clips[(size_t) h.clip];
        dragOffset = beatForX (e.position.x) - ghost.start;
        return;
    }
    drag = Drag::Select;
    dragAnchor = snapBeat (beatForX (e.position.x), free);
}

void ArrangementOverlay::mouseDrag (const juce::MouseEvent& e)
{
    if (drag == Drag::None)
        return;
    if (! dragMoved && e.getDistanceFromDragStart() < 3)
        return;
    dragMoved = true;
    const bool free = e.mods.isAltDown();
    const double beat = beatForX (e.position.x);
    switch (drag)
    {
        case Drag::Out:
        {
            drag = Drag::None;
            if (onDragOut)
            {
                const auto file = onDragOut();
                if (file.existsAsFile())
                    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this);
            }
            return;
        }
        case Drag::Select:
        {
            const double b = snapBeat (beat, free);
            setSelection (std::make_pair (juce::jmin (dragAnchor, b), juce::jmax (dragAnchor, b)));
            return;
        }
        case Drag::Move:
            ghost.start = juce::jlimit (0.0, juce::jmax (0.0, view.totalBeats - edit::minClipBeats),
                                        free ? beat - dragOffset : edit::snap (beat - dragOffset, view.gridBeats));
            break;
        case Drag::TrimStart:
        {
            const auto& c = view.clips[(size_t) dragClip];
            const double prevEnd = dragClip > 0 ? view.clips[(size_t) dragClip - 1].end() : 0.0;
            const double edge = juce::jlimit (juce::jmax (prevEnd, c.start - c.offset / c.stretch), c.end() - edit::minClipBeats, snapBeat (beat, free));
            ghost = c;
            ghost.offset += (edge - c.start) * c.stretch;
            ghost.length -= edge - c.start;
            ghost.start = edge;
            break;
        }
        case Drag::TrimEnd:
        {
            const auto& c = view.clips[(size_t) dragClip];
            const double nextStart = dragClip + 1 < (int) view.clips.size() ? view.clips[(size_t) dragClip + 1].start : view.totalBeats;
            const double latest = juce::jmin (nextStart, c.start + (view.totalBeats - c.offset) / c.stretch);
            ghost = c;
            ghost.length = juce::jlimit (edit::minClipBeats, juce::jmax (edit::minClipBeats, latest - c.start), snapBeat (beat, free) - c.start);
            break;
        }
        case Drag::Stretch:
        {
            const auto& c = view.clips[(size_t) dragClip];
            const double src = c.sourceLength();
            ghost = c;
            ghost.length = juce::jlimit (src / edit::maxStretch, juce::jmin (src / edit::minStretch, view.totalBeats - c.start), snapBeat (beat, free) - c.start);
            ghost.stretch = src / juce::jmax (edit::minClipBeats, ghost.length);
            break;
        }
        case Drag::None:
            break;
    }
    ghostActive = true;
    repaint();
}

void ArrangementOverlay::mouseUp (const juce::MouseEvent& e)
{
    const auto d = drag;
    drag = Drag::None;
    if (e.mods.isPopupMenu())
        return;
    if (! dragMoved)
    {
        if (d == Drag::Select || d == Drag::Move || d == Drag::TrimStart || d == Drag::TrimEnd)
        {
            setSelection (std::nullopt);
            if (onSeek)
                onSeek (snapBeat (beatForX (e.position.x), e.mods.isAltDown() || view.gridBeats <= 0.0));
        }
        ghostActive = false;
        repaint();
        return;
    }
    if (ghostActive && juce::isPositiveAndBelow (dragClip, (int) view.clips.size()))
    {
        switch (d)
        {
            case Drag::Move:      session.editMove (dragClip, ghost.start); break;
            case Drag::TrimStart: session.editTrim (dragClip, true, ghost.start); break;
            case Drag::TrimEnd:   session.editTrim (dragClip, false, ghost.end()); break;
            case Drag::Stretch:   session.editStretch (dragClip, ghost.end()); break;
            default: break;
        }
    }
    ghostActive = false;
    dragClip = -1;
    repaint();
}

void ArrangementOverlay::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto h = hitTest (e.position);
    if (h.clip >= 0)
    {
        const auto& c = view.clips[(size_t) h.clip];
        setSelection (std::make_pair (c.start, c.end()));
    }
}

void ArrangementOverlay::paint (juce::Graphics& g)
{
    if (timeline.lengthSamples <= 0 || view.totalBeats <= 0.0)
        return;
    const auto area = getLocalBounds().toFloat();
    const float endX = xForBeat (view.totalBeats);

    if (view.edited)
    {
        // Gaps: silence the user made. Hatched so "empty on purpose" reads differently from "quiet".
        double cursor = 0.0;
        auto gap = [&] (double a, double b)
        {
            if (b - a < 1.0e-6) return;
            const float x0 = xForBeat (a), x1 = xForBeat (b);
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (juce::Rectangle<float> (x0, area.getY(), x1 - x0, area.getHeight()).toNearestInt());
            g.setColour (colours::well.withAlpha (0.65f));
            g.fillRect (x0, area.getY(), x1 - x0, area.getHeight());
            g.setColour (colours::line);
            for (float x = x0 - area.getHeight(); x < x1; x += 7.0f)
                g.drawLine (x, area.getBottom(), x + area.getHeight(), area.getY(), 1.0f);
        };
        for (const auto& c : view.clips)
        {
            gap (cursor, c.start);
            cursor = c.end();
        }
        gap (cursor, view.totalBeats);

        // Clip name strips and boundaries.
        for (int i = 0; i < (int) view.clips.size(); ++i)
        {
            const auto& c = (ghostActive && i == dragClip) ? ghost : view.clips[(size_t) i];
            const float x0 = xForBeat (c.start), x1 = xForBeat (c.end());
            const int number = takeNumbers.count (c.take) ? takeNumbers[c.take] : 0;
            const auto tc = colours::forTake (number);
            const bool hot = hover.clip == i || (ghostActive && i == dragClip);
            const auto strip = juce::Rectangle<float> (x0, area.getY(), x1 - x0, (float) stripHeight);
            g.setColour (tc.withAlpha (hot ? 0.32f : 0.2f));
            g.fillRect (strip.reduced (0.5f, 0.0f));
            g.setColour (tc);
            g.fillRect (x0, area.getY(), 2.0f, (float) stripHeight);
            g.setColour (colours::text.withAlpha (0.22f));
            g.fillRect (x0, area.getY(), 1.0f, area.getHeight());
            g.fillRect (x1 - 1.0f, area.getY(), 1.0f, area.getHeight());
            juce::String label = number > 0 ? "Take " + juce::String (number) : juce::String ("Take");
            if (std::abs (c.stretch - 1.0) > 1.0e-6) label << "  " << ratioText (c.stretch);
            if (std::abs (c.gainDb) > 0.05f) label << "  " << (c.gainDb > 0 ? "+" : "") << juce::String (c.gainDb, 1) << " dB";
            g.setColour (colours::text.withAlpha (0.85f));
            g.setFont (font (type::meta, Weight::Medium));
            g.drawText (fitText (label, font (type::meta, Weight::Medium), strip.getWidth() - 10.0f), strip.withTrimmedLeft (6.0f),
                        juce::Justification::centredLeft, false);
        }
    }

    // Ghost outline while dragging (also for an unedited lane being trimmed).
    if (ghostActive)
    {
        const float x0 = xForBeat (ghost.start), x1 = xForBeat (ghost.end());
        g.setColour (colours::accent.withAlpha (0.12f));
        g.fillRect (x0, area.getY(), x1 - x0, area.getHeight());
        g.setColour (colours::accent);
        g.drawRect (juce::Rectangle<float> (x0, area.getY(), x1 - x0, area.getHeight()), 1.5f);
    }

    // Hovered edge
    if (hover.clip >= 0 && hover.edge != Drag::None && drag == Drag::None && juce::isPositiveAndBelow (hover.clip, (int) view.clips.size()))
    {
        const auto& c = view.clips[(size_t) hover.clip];
        const float x = xForBeat (hover.edge == Drag::TrimStart ? c.start : c.end());
        g.setColour (colours::accent);
        g.fillRoundedRectangle (x - 1.5f, area.getCentreY() - 14.0f, 3.0f, 28.0f, 1.5f);
    }

    // Selection
    if (sel || selAlpha.get() > 0.001f)
    {
        const auto s = sel.value_or (std::make_pair (0.0, 0.0));
        const float x0 = xForBeat (s.first), x1 = xForBeat (s.second);
        const float a = selAlpha.get();
        g.setColour (colours::accent.withAlpha (0.16f * a));
        g.fillRect (x0, area.getY(), x1 - x0, area.getHeight());
        g.setColour (colours::accent.withAlpha (a));
        g.fillRect (x0, area.getY(), 1.0f, area.getHeight());
        g.fillRect (x1 - 1.0f, area.getY(), 1.0f, area.getHeight());
    }

    // A fill waiting for its generation: slow moving stripes in the range.
    if (view.pendingFill)
    {
        const float x0 = xForBeat (view.pendingFill->first), x1 = xForBeat (view.pendingFill->second);
        const auto r = juce::Rectangle<float> (x0, area.getY(), x1 - x0, area.getHeight());
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (r.toNearestInt());
        g.setColour (colours::accent.withAlpha (0.10f));
        g.fillRect (r);
        const float phase = motion::enabled() ? (float) std::fmod (motion::nowMs() / 40.0, 14.0) : 0.0f;
        g.setColour (colours::accent.withAlpha (0.16f));
        for (float x = x0 - r.getHeight() - 14.0f + phase; x < x1; x += 14.0f)
            g.drawLine (x, r.getBottom(), x + r.getHeight(), r.getY(), 5.0f);
        g.setColour (colours::text);
        g.setFont (font (type::label, Weight::Medium));
        g.drawText ("New take on the way", r, juce::Justification::centred, true);
        if (motion::enabled())
            motion::Clock::instance().repaintUntil (*this, motion::nowMs() + 100.0);
    }

    // Outside the sound's length (the timeline continues with the context lane).
    if (endX < area.getRight() - 1.0f)
    {
        g.setColour (colours::background.withAlpha (0.35f));
        g.fillRect (endX, area.getY(), area.getRight() - endX, area.getHeight());
    }

    // Drag-to-DAW handle
    if (onDragOut && hasAudio() && (isMouseOver() || hover.dragHandle))
    {
        auto h = dragHandleArea();
        const float hv = motion::follow (*this, juce::Identifier ("mDragHandle"), hover.dragHandle ? 1.0f : 0.0f);
        g.setColour (colours::panel.withAlpha (0.92f).interpolatedWith (colours::accentDark, hv));
        g.fillRoundedRectangle (h, radius::control);
        g.setColour (colours::accent.withAlpha (0.4f + 0.6f * hv));
        g.drawRoundedRectangle (h.reduced (0.5f), radius::control, 1.0f);
        IconButton::drawIcon (g, IconButton::Icon::Drag, h.removeFromLeft (20.0f).withSizeKeepingCentre (8.0f, 10.0f).translated (4.0f, 0.0f), colours::accent);
        g.setColour (colours::text);
        g.setFont (font (type::meta, Weight::Medium));
        g.drawText ("Drag to DAW", h, juce::Justification::centred, false);
    }
}
} // namespace context::ui
