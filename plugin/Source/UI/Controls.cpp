#include "Controls.h"

namespace context::ui
{
juce::String fitText (const juce::String& text, const juce::Font& f, float width)
{
    if (juce::GlyphArrangement::getStringWidth (f, text) <= width)
        return text;
    const auto ellipsis = glyph ("\xe2\x80\xa6");
    int lo = 0, hi = text.length();
    while (lo < hi)
    {
        const int mid = (lo + hi + 1) / 2;
        if (juce::GlyphArrangement::getStringWidth (f, text.substring (0, mid).trimEnd() + ellipsis) <= width) lo = mid;
        else hi = mid - 1;
    }
    return text.substring (0, lo).trimEnd() + ellipsis;
}

// ===========================================================================
// SegmentedControl
// ===========================================================================
void SegmentedControl::setItems (const juce::StringArray& labels, const juce::StringArray& tooltips)
{
    items = labels;
    tips = tooltips;
    enabled.clearQuick();
    for (int i = 0; i < items.size(); ++i)
        enabled.add (true);
    if (selected >= items.size())
        selected = -1;
    resized();
    repaint();
}

void SegmentedControl::setSelected (int index, juce::NotificationType n)
{
    if (index == selected)
        return;
    selected = index;
    movePill (isShowing());
    repaint();
    if (n != juce::dontSendNotification && onChange)
        onChange (index);
}

void SegmentedControl::setItemEnabled (int index, bool e)
{
    if (juce::isPositiveAndBelow (index, enabled.size()) && enabled[index] != e)
    {
        enabled.set (index, e);
        repaint();
    }
}

void SegmentedControl::setItemTooltip (int index, const juce::String& t)
{
    while (tips.size() <= index) tips.add ({});
    tips.set (index, t);
}

int SegmentedControl::idealWidth() const
{
    const auto f = font (fontSize, Weight::Medium);
    int w = 0;
    for (auto& s : items)
        w += juce::GlyphArrangement::getStringWidthInt (f, s) + 16;
    return w + 2;
}

void SegmentedControl::resized()
{
    cells.clearQuick();
    if (items.isEmpty())
        return;
    auto r = getLocalBounds().reduced (1);
    const auto f = font (fontSize, Weight::Medium);
    float total = 0.0f;
    juce::Array<float> widths;
    for (auto& s : items)
    {
        const float w = proportional ? (float) juce::GlyphArrangement::getStringWidthInt (f, s) + 16.0f : 1.0f;
        widths.add (w);
        total += w;
    }
    float x = (float) r.getX();
    for (int i = 0; i < items.size(); ++i)
    {
        const float w = widths[i] / total * (float) r.getWidth();
        const int x0 = juce::roundToInt (x), x1 = i == items.size() - 1 ? r.getRight() : juce::roundToInt (x + w);
        cells.add ({ x0, r.getY(), x1 - x0, r.getHeight() });
        x += w;
    }
    movePill (false);
}

void SegmentedControl::movePill (bool animate)
{
    if (! juce::isPositiveAndBelow (selected, cells.size()))
        return;
    pill.onChange ([this] { repaint(); });
    const auto target = cells[selected].toFloat().reduced (1.0f);
    if (animate) pill.set (target, motion::duration::slide, motion::Ease::OutQuint);
    else         pill.snap (target);
}

void SegmentedControl::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (colours::raised);
    g.fillRoundedRectangle (r, radius::control);
    g.setColour (colours::border);
    g.drawRoundedRectangle (r, radius::control, 1.0f);
    const auto f = font (fontSize, Weight::Medium);
    g.setFont (f);
    // Hover fades per segment, then the sliding selection pill on top of the fills.
    for (int i = 0; i < cells.size(); ++i)
    {
        const bool en = isEnabled() && enabled[i];
        const float hv = motion::follow (*this, juce::Identifier ("mSeg" + juce::String (i)), (i == hovered && en && i != selected) ? 1.0f : 0.0f);
        if (hv > 0.001f)
        {
            g.setColour (colours::hover.withAlpha (hv));
            g.fillRoundedRectangle (cells[i].toFloat().reduced (1.0f), radius::small);
        }
    }
    if (juce::isPositiveAndBelow (selected, cells.size()) && pill.initialised)
    {
        g.setColour (isEnabled() ? colours::accentDark : colours::pressed);
        g.fillRoundedRectangle (pill.get(), radius::small);
    }
    const auto p = pill.get();
    for (int i = 0; i < cells.size(); ++i)
    {
        auto c = cells[i];
        const bool sel = i == selected, en = isEnabled() && enabled[i];
        // separators fade out where the pill passes
        if (i > 0 && ! (p.getX() < (float) c.getX() + 2.0f && p.getRight() > (float) c.getX() - 2.0f))
        {
            g.setColour (colours::line);
            g.fillRect (c.getX(), c.getY() + 6, 1, c.getHeight() - 12);
        }
        // text colour follows how much of the pill covers this cell
        const float cover = juce::jlimit (0.0f, 1.0f, (juce::jmin (p.getRight(), (float) c.getRight()) - juce::jmax (p.getX(), (float) c.getX())) / juce::jmax (1.0f, (float) c.getWidth()));
        const auto base = (i == hovered && en) ? colours::text : colours::dim;
        g.setColour (! en ? colours::disabled.brighter (0.15f) : base.interpolatedWith (colours::accent, sel ? juce::jmax (cover, 0.0f) : cover * 0.6f));
        g.drawText (fitText (items[i], f, (float) c.getWidth() - 6.0f), c, juce::Justification::centred, false);
    }
}

int SegmentedControl::indexAt (juce::Point<int> p) const
{
    for (int i = 0; i < cells.size(); ++i)
        if (cells[i].contains (p))
            return i;
    return -1;
}

void SegmentedControl::mouseDown (const juce::MouseEvent& e)
{
    const int i = indexAt (e.getPosition());
    if (i >= 0 && isEnabled() && enabled[i] && i != selected)
        setSelected (i, juce::sendNotificationSync);
}

void SegmentedControl::mouseMove (const juce::MouseEvent& e)
{
    const int i = indexAt (e.getPosition());
    if (i != hovered) { hovered = i; repaint(); }
}

void SegmentedControl::mouseExit (const juce::MouseEvent&)
{
    hovered = -1;
    repaint();
}

juce::String SegmentedControl::getTooltip()
{
    const int i = indexAt (getMouseXYRelative());
    return juce::isPositiveAndBelow (i, tips.size()) ? tips[i] : juce::String();
}

// ===========================================================================
// LaneToggle / IconButton
// ===========================================================================
LaneToggle::LaneToggle (const juce::String& letter, juce::Colour onColour) : juce::Button (letter), on (onColour)
{
    setClickingTogglesState (true);
}

void LaneToggle::paintButton (juce::Graphics& g, bool over, bool down)
{
    static const juce::Identifier onKey ("mOn");
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    const float act = motion::follow (*this, onKey, getToggleState() ? 1.0f : 0.0f, motion::duration::toggle);
    const float hv = motion::hoverAmount (*this, over), pr = motion::pressAmount (*this, down);
    const auto idle = colours::raised.interpolatedWith (colours::hover, hv).interpolatedWith (colours::pressed, pr);
    g.setColour (idle.interpolatedWith (on, act));
    g.fillRoundedRectangle (r, radius::small);
    g.setColour (colours::border.withAlpha (1.0f - act));
    g.drawRoundedRectangle (r.reduced (0.5f), radius::small, 1.0f);
    g.setColour (colours::dim.interpolatedWith (colours::text, hv).interpolatedWith (colours::accentText, act));
    g.setFont (font (type::meta, Weight::SemiBold));
    g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred);
}

IconButton::IconButton (Icon i, const juce::String& tip) : juce::Button (tip), icon (i)
{
    setTooltip (tip);
}

void IconButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    static const juce::Identifier onKey ("mOn");
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    const bool en = isEnabled();
    const float hv = motion::hoverAmount (*this, en && over), pr = motion::pressAmount (*this, en && down);
    const float on = motion::follow (*this, onKey, getToggleState() ? 1.0f : 0.0f, motion::duration::toggle);
    const auto style = (ButtonStyle) (int) getProperties().getWithDefault ("ctxStyle", (int) ButtonStyle::Ghost);
    if (style == ButtonStyle::Primary)
    {
        g.setColour (! en ? colours::raised : colours::accent.interpolatedWith (colours::accentHi, hv).interpolatedWith (colours::accent.darker (0.2f), pr));
        g.fillRoundedRectangle (r, radius::control);
    }
    else
    {
        const auto base = style == ButtonStyle::Secondary ? colours::raised : colours::hover.withAlpha (0.0f);
        const auto fill = base.interpolatedWith (colours::hover, hv).interpolatedWith (colours::pressed, pr).interpolatedWith (colours::accentDark, on);
        if (fill.getAlpha() > 0)
        {
            g.setColour (fill);
            g.fillRoundedRectangle (r, radius::control);
        }
        if (style == ButtonStyle::Secondary)
        {
            g.setColour (colours::border.withAlpha (1.0f - on));
            g.drawRoundedRectangle (r.reduced (0.5f), radius::control, 1.0f);
        }
    }
    juce::Colour c = style == ButtonStyle::Primary ? colours::accentText
                                                   : colours::dim.interpolatedWith (colours::text, hv).interpolatedWith (onColour, on);
    if (! en) c = colours::disabled.brighter (0.2f);
    const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f;
    drawIcon (g, icon, r.withSizeKeepingCentre (s, s).translated (0.0f, 0.5f * pr), c);
}

void IconButton::drawIcon (juce::Graphics& g, Icon icon, juce::Rectangle<float> r, juce::Colour c)
{
    g.setColour (c);
    juce::Path p;
    const float x = r.getX(), y = r.getY(), w = r.getWidth(), h = r.getHeight();
    const juce::PathStrokeType stroke (juce::jmax (1.3f, w * 0.12f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    switch (icon)
    {
        case Icon::Play:
            p.addTriangle (x + w * 0.18f, y, x + w * 0.18f, y + h, x + w, y + h * 0.5f);
            g.fillPath (p);
            break;
        case Icon::Stop:
            g.fillRoundedRectangle (r.reduced (w * 0.08f), 1.5f);
            break;
        case Icon::Rewind:
            g.fillRect (x, y + h * 0.05f, w * 0.12f, h * 0.9f);
            p.addTriangle (x + w, y + h * 0.05f, x + w, y + h * 0.95f, x + w * 0.18f, y + h * 0.5f);
            g.fillPath (p);
            break;
        case Icon::Record:
            g.fillEllipse (r.reduced (w * 0.12f));
            break;
        case Icon::Loop:
            p.addCentredArc (r.getCentreX(), r.getCentreY(), w * 0.45f, h * 0.36f, 0.0f, 0.5f, juce::MathConstants<float>::twoPi - 0.35f, true);
            g.strokePath (p, stroke);
            {
                juce::Path a;
                const float ax = r.getCentreX() + std::sin (0.5f) * w * 0.45f, ay = r.getCentreY() - std::cos (0.5f) * h * 0.36f;
                a.addTriangle (ax - w * 0.2f, ay - h * 0.16f, ax + w * 0.06f, ay + h * 0.02f, ax - w * 0.16f, ay + h * 0.2f);
                g.fillPath (a);
            }
            break;
        case Icon::Power:
            p.addCentredArc (r.getCentreX(), r.getCentreY() + h * 0.05f, w * 0.42f, h * 0.42f, 0.0f, 0.65f, juce::MathConstants<float>::twoPi - 0.65f, true);
            p.startNewSubPath (r.getCentreX(), y);
            p.lineTo (r.getCentreX(), r.getCentreY());
            g.strokePath (p, stroke);
            break;
        case Icon::Plus:
            p.startNewSubPath (r.getCentreX(), y + h * 0.1f); p.lineTo (r.getCentreX(), y + h * 0.9f);
            p.startNewSubPath (x + w * 0.1f, r.getCentreY()); p.lineTo (x + w * 0.9f, r.getCentreY());
            g.strokePath (p, stroke);
            break;
        case Icon::Check:
            p.startNewSubPath (x + w * 0.12f, y + h * 0.55f);
            p.lineTo (x + w * 0.4f, y + h * 0.82f);
            p.lineTo (x + w * 0.9f, y + h * 0.2f);
            g.strokePath (p, stroke);
            break;
        case Icon::Close:
            p.startNewSubPath (x + w * 0.15f, y + h * 0.15f); p.lineTo (x + w * 0.85f, y + h * 0.85f);
            p.startNewSubPath (x + w * 0.85f, y + h * 0.15f); p.lineTo (x + w * 0.15f, y + h * 0.85f);
            g.strokePath (p, stroke);
            break;
        case Icon::Folder:
            p.startNewSubPath (x, y + h * 0.15f);
            p.lineTo (x + w * 0.38f, y + h * 0.15f);
            p.lineTo (x + w * 0.5f, y + h * 0.3f);
            p.lineTo (x + w, y + h * 0.3f);
            p.lineTo (x + w, y + h * 0.9f);
            p.lineTo (x, y + h * 0.9f);
            p.closeSubPath();
            g.strokePath (p, stroke);
            break;
        case Icon::Copy:
            g.drawRoundedRectangle (juce::Rectangle<float> (x + w * 0.3f, y, w * 0.7f, h * 0.7f), 1.5f, stroke.getStrokeThickness());
            g.drawRoundedRectangle (juce::Rectangle<float> (x, y + h * 0.3f, w * 0.7f, h * 0.7f), 1.5f, stroke.getStrokeThickness());
            break;
        case Icon::Save:
            p.startNewSubPath (r.getCentreX(), y); p.lineTo (r.getCentreX(), y + h * 0.62f);
            p.startNewSubPath (x + w * 0.22f, y + h * 0.36f); p.lineTo (r.getCentreX(), y + h * 0.62f); p.lineTo (x + w * 0.78f, y + h * 0.36f);
            p.startNewSubPath (x, y + h * 0.7f); p.lineTo (x, y + h); p.lineTo (x + w, y + h); p.lineTo (x + w, y + h * 0.7f);
            g.strokePath (p, stroke);
            break;
        case Icon::Star:
        case Icon::StarFilled:
        {
            p.addStar (r.getCentre(), 5, w * 0.22f, w * 0.52f, 0.0f);
            if (icon == Icon::StarFilled) g.fillPath (p);
            else g.strokePath (p, juce::PathStrokeType (juce::jmax (1.2f, w * 0.09f), juce::PathStrokeType::mitered));
            break;
        }
        case Icon::Drag:
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 2; ++col)
                    g.fillEllipse (x + w * (0.25f + 0.4f * col) - 1.5f, y + h * (0.15f + 0.35f * row) - 1.5f, 3.0f, 3.0f);
            break;
        case Icon::More:
            for (int i = 0; i < 3; ++i)
                g.fillEllipse (x + w * (0.15f + 0.35f * i) - 1.5f, r.getCentreY() - 1.5f, 3.0f, 3.0f);
            break;
        case Icon::Refresh:
            p.addCentredArc (r.getCentreX(), r.getCentreY(), w * 0.42f, h * 0.42f, 0.0f, 0.6f, juce::MathConstants<float>::twoPi - 0.1f, true);
            g.strokePath (p, stroke);
            {
                juce::Path a;
                const float ax = r.getCentreX() + std::sin (0.6f) * w * 0.42f, ay = r.getCentreY() - std::cos (0.6f) * h * 0.42f;
                a.addTriangle (ax - w * 0.25f, ay - h * 0.12f, ax + w * 0.05f, ay - h * 0.2f, ax - w * 0.05f, ay + h * 0.15f);
                g.fillPath (a);
            }
            break;
        case Icon::Undo:
        case Icon::Redo:
        {
            const bool redo = icon == Icon::Redo;
            auto mx = [&] (float fx) { return redo ? x + w - fx * w : x + fx * w; };
            p.startNewSubPath (mx (0.12f), y + h * 0.42f);
            p.lineTo (mx (0.62f), y + h * 0.42f);
            p.cubicTo (mx (1.0f), y + h * 0.42f, mx (1.0f), y + h * 0.95f, mx (0.62f), y + h * 0.95f);
            p.lineTo (mx (0.35f), y + h * 0.95f);
            p.startNewSubPath (mx (0.38f), y + h * 0.14f);
            p.lineTo (mx (0.1f), y + h * 0.42f);
            p.lineTo (mx (0.38f), y + h * 0.7f);
            g.strokePath (p, stroke);
            break;
        }
        case Icon::Split:
            p.startNewSubPath (r.getCentreX(), y); p.lineTo (r.getCentreX(), y + h);
            p.startNewSubPath (x, y + h * 0.3f); p.lineTo (x + w * 0.32f, y + h * 0.3f);
            p.startNewSubPath (x + w * 0.68f, y + h * 0.7f); p.lineTo (x + w, y + h * 0.7f);
            g.strokePath (p, stroke);
            break;
        case Icon::Trash:
            p.startNewSubPath (x + w * 0.05f, y + h * 0.2f); p.lineTo (x + w * 0.95f, y + h * 0.2f);
            p.startNewSubPath (x + w * 0.35f, y + h * 0.2f); p.lineTo (x + w * 0.38f, y); p.lineTo (x + w * 0.62f, y); p.lineTo (x + w * 0.65f, y + h * 0.2f);
            p.startNewSubPath (x + w * 0.16f, y + h * 0.2f); p.lineTo (x + w * 0.24f, y + h); p.lineTo (x + w * 0.76f, y + h); p.lineTo (x + w * 0.84f, y + h * 0.2f);
            g.strokePath (p, stroke);
            break;
        case Icon::Keys:
            g.drawRoundedRectangle (r.reduced (0.0f, h * 0.15f), 1.5f, stroke.getStrokeThickness());
            for (int i = 0; i < 3; ++i)
                g.fillRect (x + w * (0.2f + 0.25f * i), y + h * 0.36f, w * 0.12f, h * 0.1f);
            g.fillRect (x + w * 0.25f, y + h * 0.58f, w * 0.5f, h * 0.1f);
            break;
        case Icon::Sparkle:
        {
            auto star = [&] (float cx, float cy, float rr)
            {
                juce::Path s4;
                s4.startNewSubPath (cx, cy - rr);
                s4.quadraticTo (cx, cy, cx + rr, cy);
                s4.quadraticTo (cx, cy, cx, cy + rr);
                s4.quadraticTo (cx, cy, cx - rr, cy);
                s4.quadraticTo (cx, cy, cx, cy - rr);
                s4.closeSubPath();
                g.fillPath (s4);
            };
            star (x + w * 0.42f, y + h * 0.55f, w * 0.42f);
            star (x + w * 0.84f, y + h * 0.16f, w * 0.18f);
            break;
        }
        case Icon::Takes:
            for (int i = 0; i < 3; ++i)
                g.fillRoundedRectangle (juce::Rectangle<float> (x + w * 0.08f * (float) (2 - i), y + h * (0.12f + 0.3f * (float) i), w * (0.84f - 0.0f), h * 0.18f), 1.0f);
            break;
        case Icon::Magnet:
            p.startNewSubPath (x + w * 0.15f, y);
            p.lineTo (x + w * 0.15f, y + h * 0.55f);
            p.addCentredArc (r.getCentreX(), y + h * 0.55f, w * 0.35f, h * 0.4f, 0.0f, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, false);
            p.lineTo (x + w * 0.85f, y);
            g.strokePath (p, stroke);
            break;
        case Icon::Help:
            p.addCentredArc (r.getCentreX(), y + h * 0.32f, w * 0.26f, h * 0.24f, 0.0f, -juce::MathConstants<float>::halfPi * 1.2f, juce::MathConstants<float>::halfPi * 1.1f, true);
            p.lineTo (r.getCentreX(), y + h * 0.68f);
            g.strokePath (p, stroke);
            g.fillEllipse (r.getCentreX() - w * 0.08f, y + h * 0.84f, w * 0.16f, w * 0.16f);
            break;
        case Icon::Variation:
            p.startNewSubPath (x, y + h * 0.8f);
            p.cubicTo (x + w * 0.3f, y + h * 0.8f, x + w * 0.4f, y + h * 0.2f, x + w * 0.7f, y + h * 0.2f);
            p.lineTo (x + w, y + h * 0.2f);
            p.startNewSubPath (x, y + h * 0.2f);
            p.cubicTo (x + w * 0.3f, y + h * 0.2f, x + w * 0.4f, y + h * 0.8f, x + w * 0.7f, y + h * 0.8f);
            p.lineTo (x + w, y + h * 0.8f);
            g.strokePath (p, stroke);
            break;
    }
}

// ===========================================================================
// Parameter controls
// ===========================================================================
Knob::Knob (fx::EffectParameter& p) : ParameterControl (p)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    const auto range = p.getRange();
    slider.setNormalisableRange ({ (double) range.start, (double) range.end,
                                   [range] (double, double, double n) { return (double) range.convertFrom0to1 ((float) n); },
                                   [range] (double, double, double v) { return (double) range.convertTo0to1 ((float) v); },
                                   [&p] (double, double, double v) { return (double) p.clamp ((float) v); } });
    slider.setDoubleClickReturnValue (true, p.spec().defaultValue);
    slider.setMouseDragSensitivity (220);
    slider.setVelocityModeParameters (0.4, 1, 0.0, true, juce::ModifierKeys::shiftModifier);   // Shift = fine
    slider.setValue (p.get(), juce::dontSendNotification);
    slider.setTooltip (p.spec().tooltip);
    slider.onDragStart = [this] { if (onGestureStart) onGestureStart(); };
    slider.onDragEnd = [this] { if (onGestureEnd) onGestureEnd(); };
    slider.textFromValueFunction = [&p] (double v) { return p.toText ((float) v); };
    slider.onValueChange = [this]
    {
        if (syncing) return;
        param.set ((float) slider.getValue());
        value.setText (param.currentText(), juce::dontSendNotification);
        if (onEdit) onEdit();
    };
    addAndMakeVisible (slider);

    value.setJustificationType (juce::Justification::centred);
    value.setFont (numberFont (type::meta));
    value.setColour (juce::Label::textColourId, colours::dim);
    value.setEditable (false, true, false);
    value.setTooltip ("Double-click to type a value");
    value.onTextChange = [this]
    {
        if (auto v = param.fromText (value.getText()))
            slider.setValue (*v, juce::sendNotificationSync);
        value.setText (param.currentText(), juce::dontSendNotification);
    };
    value.setText (p.currentText(), juce::dontSendNotification);
    addAndMakeVisible (value);
}

void Knob::refresh (bool relevant)
{
    setEnabled (relevant);
    if (std::abs (slider.getValue() - param.get()) > 1.0e-6)
    {
        syncing = true;
        slider.setValue (param.get(), juce::dontSendNotification);
        syncing = false;
    }
    if (! value.isBeingEdited())
        value.setText (param.currentText(), juce::dontSendNotification);
    value.setColour (juce::Label::textColourId, relevant ? colours::dim : colours::disabled);
    repaint();
}

void Knob::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (15);
    value.setBounds (r.removeFromBottom (15));
    const int d = juce::jmin (r.getWidth(), r.getHeight());
    slider.setBounds (r.withSizeKeepingCentre (d, d));
}

void Knob::paint (juce::Graphics& g)
{
    g.setColour (isEnabled() ? colours::dim : colours::disabled);
    g.setFont (font (type::label, Weight::Medium));
    g.drawText (fitText (param.spec().name, font (type::label, Weight::Medium), (float) getWidth()), getLocalBounds().removeFromTop (14), juce::Justification::centred, false);
}

ChoiceControl::ChoiceControl (fx::EffectParameter& p) : ParameterControl (p)
{
    const auto& choices = p.spec().choices;
    int totalChars = 0;
    for (auto& c : choices) totalChars += c.length();
    useSegments = choices.size() <= 4 && totalChars <= 26;
    if (useSegments)
    {
        segments.setItems (choices);
        segments.setProportional (true);
        segments.setFontSize (type::label);
        segments.setSelected (p.getIndex());
        segments.onChange = [this] (int i) { param.set ((float) i); if (onEdit) onEdit(); };
        addAndMakeVisible (segments);
    }
    else
    {
        for (int i = 0; i < choices.size(); ++i)
            box.addItem (choices[i], i + 1);
        box.setSelectedId (p.getIndex() + 1, juce::dontSendNotification);
        box.onChange = [this] { param.set ((float) (box.getSelectedId() - 1)); if (onEdit) onEdit(); };
        addAndMakeVisible (box);
    }
    setTooltip (p.spec().tooltip);
}

int ChoiceControl::preferredWidth() const
{
    if (useSegments)
        return juce::jmax (80, segments.idealWidth());
    return 108;
}

void ChoiceControl::refresh (bool relevant)
{
    setEnabled (relevant);
    if (useSegments) segments.setSelected (param.getIndex());
    else box.setSelectedId (param.getIndex() + 1, juce::dontSendNotification);
    repaint();
}

void ChoiceControl::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (15);
    auto row = r.withSizeKeepingCentre (r.getWidth(), height::control);
    if (useSegments) segments.setBounds (row);
    else box.setBounds (row);
}

void ChoiceControl::paint (juce::Graphics& g)
{
    g.setColour (isEnabled() ? colours::dim : colours::disabled);
    g.setFont (font (type::label, Weight::Medium));
    g.drawText (param.spec().name, getLocalBounds().removeFromTop (14), juce::Justification::centred, true);
}

ToggleControl::ToggleControl (fx::EffectParameter& p) : ParameterControl (p)
{
    button.setButtonText (p.spec().name);
    button.setClickingTogglesState (true);
    setStyle (button, ButtonStyle::Toggle);
    button.setToggleState (p.getBool(), juce::dontSendNotification);
    button.setTooltip (p.spec().tooltip);
    button.onClick = [this] { param.set (button.getToggleState() ? 1.0f : 0.0f); if (onEdit) onEdit(); };
    addAndMakeVisible (button);
}

void ToggleControl::refresh (bool relevant)
{
    setEnabled (relevant);
    button.setToggleState (param.getBool(), juce::dontSendNotification);
}

void ToggleControl::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (15);
    button.setBounds (r.withSizeKeepingCentre (r.getWidth(), height::control));
}

void ToggleControl::paint (juce::Graphics&) {}

std::unique_ptr<ParameterControl> makeParameterControl (fx::EffectParameter& p)
{
    if (p.isChoice()) return std::make_unique<ChoiceControl> (p);
    if (p.isToggle()) return std::make_unique<ToggleControl> (p);
    return std::make_unique<Knob> (p);
}
} // namespace context::ui
