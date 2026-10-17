#include "Theme.h"
#include "ContextAssets.h"
#include "Motion.h"

namespace context::ui
{
namespace
{
    juce::Typeface::Ptr typeface (Weight w)
    {
        // Created once per process from the embedded font data; immutable afterwards.
        static const juce::Typeface::Ptr regular  = juce::Typeface::createSystemTypefaceFor (ContextAssets::BarlowRegular_ttf, ContextAssets::BarlowRegular_ttfSize);
        static const juce::Typeface::Ptr medium   = juce::Typeface::createSystemTypefaceFor (ContextAssets::BarlowMedium_ttf, ContextAssets::BarlowMedium_ttfSize);
        static const juce::Typeface::Ptr semibold = juce::Typeface::createSystemTypefaceFor (ContextAssets::BarlowSemiBold_ttf, ContextAssets::BarlowSemiBold_ttfSize);
        switch (w)
        {
            case Weight::Medium:   return medium != nullptr ? medium : regular;
            case Weight::SemiBold: return semibold != nullptr ? semibold : regular;
            case Weight::Regular:  break;
        }
        return regular;
    }

    ButtonStyle styleOf (const juce::Component& c)
    {
        return (ButtonStyle) (int) c.getProperties().getWithDefault ("ctxStyle", (int) ButtonStyle::Secondary);
    }
}

juce::Font font (float size, Weight w)
{
    if (auto tf = typeface (w))
        return juce::Font (juce::FontOptions (tf).withHeight (size));
    return juce::Font (juce::FontOptions (size, w == Weight::Regular ? juce::Font::plain : juce::Font::bold));
}

juce::Font numberFont (float size, Weight w)
{
    if (auto tf = typeface (w))
        return juce::Font (juce::FontOptions (tf).withHeight (size).withFeatureEnabled (juce::FontFeatureTag ("tnum")));
    return font (size, w);
}

juce::Font labelFont()
{
    return font (type::label, Weight::Medium);
}

juce::Colour colours::forTake (int number)
{
    // Muted, distinguishable hues that sit with the graphite; take 1 is the house teal.
    static const juce::uint32 hues[] = { 0xff43c7b8, 0xff8fa8e8, 0xffd59fd4, 0xffd8b56a, 0xff86c98a, 0xffe08f86 };
    return juce::Colour (hues[(size_t) juce::jmax (0, number - 1) % std::size (hues)]);
}

void setStyle (juce::Button& b, ButtonStyle s)
{
    b.getProperties().set ("ctxStyle", (int) s);
    b.repaint();
}

juce::Font ContextLookAndFeel::font (float height, bool bold)
{
    return ui::font (height, bold ? Weight::SemiBold : Weight::Regular);
}

ContextLookAndFeel::ContextLookAndFeel()
{
    using namespace colours;
    setColour (juce::ResizableWindow::backgroundColourId, background);
    setColour (juce::DocumentWindow::backgroundColourId, background);
    setColour (juce::TextButton::buttonColourId, raised);
    setColour (juce::TextButton::buttonOnColourId, accentDark);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, accent);
    setColour (juce::ToggleButton::textColourId, dim);
    setColour (juce::ToggleButton::tickColourId, accent);
    setColour (juce::ToggleButton::tickDisabledColourId, disabled);
    setColour (juce::Label::textColourId, text);
    setColour (juce::TextEditor::backgroundColourId, well);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.28f));
    setColour (juce::TextEditor::highlightedTextColourId, text);
    setColour (juce::TextEditor::outlineColourId, border);
    setColour (juce::TextEditor::focusedOutlineColourId, accent.withAlpha (0.7f));
    setColour (juce::CaretComponent::caretColourId, accent);
    setColour (juce::ComboBox::backgroundColourId, raised);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::outlineColourId, border);
    setColour (juce::ComboBox::arrowColourId, faint);
    setColour (juce::PopupMenu::backgroundColourId, panel);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::headerTextColourId, faint);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, hover);
    setColour (juce::PopupMenu::highlightedTextColourId, text);
    setColour (juce::ListBox::backgroundColourId, panel);
    setColour (juce::ListBox::outlineColourId, line);
    setColour (juce::ScrollBar::thumbColourId, border);
    setColour (juce::Slider::thumbColourId, text);
    setColour (juce::Slider::trackColourId, accent);
    setColour (juce::Slider::backgroundColourId, raised);
    setColour (juce::Slider::rotarySliderFillColourId, accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, border);
    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxBackgroundColourId, well);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, accent.withAlpha (0.3f));
    setColour (juce::ProgressBar::backgroundColourId, raised);
    setColour (juce::ProgressBar::foregroundColourId, accent);
    setColour (juce::AlertWindow::backgroundColourId, panel);
    setColour (juce::AlertWindow::textColourId, text);
    setColour (juce::AlertWindow::outlineColourId, border);
    setColour (juce::TooltipWindow::backgroundColourId, raised);
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, border);
    setDefaultSansSerifTypeface (typeface (Weight::Regular));
}

juce::Typeface::Ptr ContextLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    // Every font in the plugin resolves to the embedded Barlow (consistent on every OS).
    if (f.getTypefacePtr() != nullptr && f.getTypefaceName() != juce::Font::getDefaultSansSerifFontName())
        return f.getTypefacePtr();
    return typeface (f.isBold() ? Weight::SemiBold : Weight::Regular);
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

juce::Font ContextLookAndFeel::getTextButtonFont (juce::TextButton& b, int h)
{
    const float size = juce::jlimit (12.0f, 14.5f, (float) h * 0.54f);
    return ui::font (size, styleOf (b) == ButtonStyle::Primary ? Weight::SemiBold : Weight::Medium);
}

void ContextLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    using namespace colours;
    static const juce::Identifier onKey ("mOn");
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool enabled = b.isEnabled();
    const auto style = styleOf (b);
    // Animated state: hover / press fade, toggle fills cross-fade.
    const float hv = motion::hoverAmount (b, enabled && over);
    const float pr = motion::pressAmount (b, enabled && down);
    const float on = motion::follow (b, onKey, b.getToggleState() ? 1.0f : 0.0f, motion::duration::toggle);
    auto mix = [] (juce::Colour a, juce::Colour c, float t) { return a.interpolatedWith (c, juce::jlimit (0.0f, 1.0f, t)); };

    juce::Colour fill, outline = juce::Colours::transparentBlack;
    switch (style)
    {
        case ButtonStyle::Primary:
            fill = ! enabled ? raised : mix (mix (accent, accentHi, hv), accent.darker (0.2f), pr);
            break;
        case ButtonStyle::Danger:
            fill = ! enabled ? raised : mix (mix (record.withAlpha (0.85f), record.brighter (0.1f), hv), record.darker (0.2f), pr);
            break;
        case ButtonStyle::Ghost:
            fill = mix (mix (hover.withAlpha (0.0f), hover, hv), pressed, pr);
            fill = mix (fill, accentDark.withAlpha (0.7f), on);
            break;
        case ButtonStyle::Tab:
            fill = juce::Colours::transparentBlack;   // the owner draws the moving underline
            break;
        case ButtonStyle::Nav:
            fill = mix (mix (hover.withAlpha (0.0f), hover, hv * 0.8f), raised, on);
            break;
        case ButtonStyle::Toggle:
            fill = enabled ? mix (mix (mix (raised, hover, hv), pressed, pr), accentDark, on) : raised.withAlpha (0.5f);
            outline = mix (border, accent.withAlpha (0.55f), on);
            break;
        case ButtonStyle::Secondary:
            fill = enabled ? mix (mix (raised, hover, hv), pressed, pr) : raised.withAlpha (0.5f);
            outline = enabled ? mix (border, border.brighter (0.25f), hv) : line;
            break;
    }
    // A press sinks the control by half a pixel: tactile without moving the layout.
    r = r.translated (0.0f, 0.5f * pr);
    g.setColour (fill);
    g.fillRoundedRectangle (r, radius::control);
    if (style == ButtonStyle::Nav && on > 0.001f)
    {
        g.setColour (accent.withAlpha (on));
        g.fillRoundedRectangle (r.withWidth (2.5f).reduced (0.0f, r.getHeight() * 0.22f), 1.0f);
    }
    if (! outline.isTransparent())
    {
        g.setColour (outline);
        g.drawRoundedRectangle (r.reduced (0.5f), radius::control, 1.0f);
    }
    if (b.hasKeyboardFocus (false) && b.isShowing() && style != ButtonStyle::Ghost && style != ButtonStyle::Tab && style != ButtonStyle::Nav)
    {
        g.setColour (accent.withAlpha (0.55f));
        g.drawRoundedRectangle (r.expanded (1.0f), radius::control + 1.0f, 1.2f);
    }
}

void ContextLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool over, bool down)
{
    using namespace colours;
    static const juce::Identifier onKey ("mOn");
    const auto style = styleOf (b);
    const float hv = motion::hoverAmount (b, b.isEnabled() && over);
    const float on = motion::follow (b, onKey, b.getToggleState() ? 1.0f : 0.0f, motion::duration::toggle);
    juce::Colour c = text.interpolatedWith (accent, on);
    if (style == ButtonStyle::Primary) c = b.isEnabled() ? accentText : disabled.brighter (0.3f);
    else if (style == ButtonStyle::Danger) c = juce::Colours::white;
    else if (style == ButtonStyle::Ghost) c = dim.interpolatedWith (text, hv).interpolatedWith (accent, on);
    else if (style == ButtonStyle::Tab) c = faint.interpolatedWith (text, juce::jmax (hv, on));
    else if (style == ButtonStyle::Nav) c = dim.interpolatedWith (text, juce::jmax (hv, on));
    if (b.findColour (juce::TextButton::textColourOffId) != text && ! b.getToggleState() && style != ButtonStyle::Primary)
        c = b.findColour (juce::TextButton::textColourOffId);   // explicit per-button colour (e.g. favourite)
    if (! b.isEnabled())
        c = disabled.brighter (0.25f);
    g.setColour (c);
    g.setFont (getTextButtonFont (b, b.getHeight()));
    const float sink = 0.5f * motion::pressAmount (b, b.isEnabled() && down);
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (style == ButtonStyle::Nav ? 12 : 6, 1).translated (0, juce::roundToInt (sink)),
                      style == ButtonStyle::Nav ? juce::Justification::centredLeft : juce::Justification::centred, 1, 0.85f);
}

void ContextLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool)
{
    // A small switch: the knob slides, the track fills with teal.
    using namespace colours;
    static const juce::Identifier onKey ("mOn");
    const auto r = b.getLocalBounds();
    const float on = motion::follow (b, onKey, b.getToggleState() ? 1.0f : 0.0f, motion::duration::toggle, motion::Ease::OutCubic);
    const float hv = motion::hoverAmount (b, b.isEnabled() && over);
    const float alpha = b.isEnabled() ? 1.0f : 0.4f;
    const float tw = 26.0f, th = 14.0f;
    const auto track = juce::Rectangle<float> (1.0f, (r.getHeight() - th) * 0.5f, tw, th);
    g.setColour (raised.interpolatedWith (accentDark, on).brighter (0.06f * hv).withAlpha (alpha));
    g.fillRoundedRectangle (track, th * 0.5f);
    g.setColour (border.interpolatedWith (accent.withAlpha (0.7f), on).withAlpha (alpha));
    g.drawRoundedRectangle (track.reduced (0.5f), th * 0.5f, 1.0f);
    const float k = th - 4.0f;
    const float kx = track.getX() + 2.0f + on * (tw - k - 4.0f);
    g.setColour (dim.interpolatedWith (accent, on).brighter (0.1f * hv).withAlpha (alpha));
    g.fillEllipse (kx, track.getY() + 2.0f, k, k);
    g.setColour (dim.interpolatedWith (text, hv).withAlpha (alpha));
    g.setFont (ui::font (type::body));
    g.drawFittedText (b.getButtonText(), r.withTrimmedLeft ((int) tw + 9), juce::Justification::centredLeft, 1, 0.85f);
}

// ---------------------------------------------------------------------------
// Text fields, combo boxes, menus, tooltips
// ---------------------------------------------------------------------------

void ContextLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    g.setColour (e.findColour (juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle (juce::Rectangle<float> (0, 0, (float) w, (float) h), radius::control);
}

void ContextLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    const bool focus = e.hasKeyboardFocus (true) && ! e.isReadOnly();
    g.setColour (e.findColour (focus ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId));
    g.drawRoundedRectangle (juce::Rectangle<float> (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f), radius::control, 1.0f);
}

void drawProgress (juce::Graphics& g, juce::Rectangle<float> r, double progress)
{
    const float h = r.getHeight();
    g.setColour (colours::raised);
    g.fillRoundedRectangle (r, h * 0.5f);
    g.setColour (colours::accent);
    if (progress >= 0.0 && progress <= 1.0)
    {
        g.fillRoundedRectangle (r.withWidth (juce::jmax (h, r.getWidth() * (float) progress)), h * 0.5f);
    }
    else
    {
        const auto t = (float) ((juce::Time::getMillisecondCounter() % 1400) / 1400.0);
        const float segW = r.getWidth() * 0.25f;
        g.fillRoundedRectangle (juce::Rectangle<float> (r.getX() - segW + t * (r.getWidth() + segW), r.getY(), segW, h).getIntersection (r), h * 0.5f);
    }
}

void ContextLookAndFeel::drawProgressBar (juce::Graphics& g, juce::ProgressBar&, int w, int h, double progress, const juce::String&)
{
    drawProgress (g, juce::Rectangle<float> (0, 0, (float) w, (float) h), progress);
}

void ContextLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool down, int, int, int, int, juce::ComboBox& box)
{
    using namespace colours;
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    const bool over = box.isMouseOver (true);
    const float hv = motion::hoverAmount (box, box.isEnabled() && over);
    g.setColour (! box.isEnabled() ? raised.withAlpha (0.5f) : down ? pressed : raised.interpolatedWith (hover, hv));
    g.fillRoundedRectangle (r, radius::control);
    g.setColour (border);
    g.drawRoundedRectangle (r.reduced (0.5f), radius::control, 1.0f);
    juce::Path chevron;
    const float cx = (float) w - 11.0f, cy = (float) h * 0.5f;
    chevron.startNewSubPath (cx - 3.5f, cy - 1.5f);
    chevron.lineTo (cx, cy + 2.0f);
    chevron.lineTo (cx + 3.5f, cy - 1.5f);
    g.setColour (box.isEnabled() ? (over ? dim : faint) : disabled);
    g.strokePath (chevron, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font ContextLookAndFeel::getComboBoxFont (juce::ComboBox& b)
{
    return ui::font (juce::jlimit (12.0f, 13.5f, (float) b.getHeight() * 0.54f), Weight::Medium);
}

void ContextLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (7, 1, box.getWidth() - 24, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

void ContextLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    g.fillAll (colours::panel);
    g.setColour (colours::border);
    g.drawRect (0, 0, w, h, 1);
}

juce::Font ContextLookAndFeel::getPopupMenuFont()
{
    return ui::font (type::body);
}

juce::PopupMenu::Options ContextLookAndFeel::getOptionsForComboBoxPopupMenu (juce::ComboBox& box, juce::Label& label)
{
    return LookAndFeel_V4::getOptionsForComboBoxPopupMenu (box, label).withStandardItemHeight (26);
}

void ContextLookAndFeel::drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics& g, const juce::Path& path, juce::Image&)
{
    g.setColour (colours::panel);
    g.fillPath (path);
    g.setColour (colours::border);
    g.strokePath (path, juce::PathStrokeType (1.0f));
}

void ContextLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int, int& w, int& h)
{
    if (isSeparator)
    {
        w = 50;
        h = 7;
        return;
    }
    h = 26;
    w = juce::GlyphArrangement::getStringWidthInt (getPopupMenuFont(), text) + 52;
}

void ContextLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                                            bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcut,
                                            const juce::Drawable*, const juce::Colour* textColour)
{
    using namespace colours;
    if (isSeparator)
    {
        g.setColour (line);
        g.fillRect (area.reduced (8, 0).withHeight (1).withY (area.getCentreY()));
        return;
    }
    if (isHighlighted && isActive)
    {
        g.setColour (hover);
        g.fillRoundedRectangle (area.reduced (3, 1).toFloat(), radius::control);
    }
    auto r = area.reduced (10, 0);
    if (isTicked)
    {
        g.setColour (accent);
        g.fillEllipse ((float) r.getX(), (float) area.getCentreY() - 2.5f, 5.0f, 5.0f);
    }
    r.removeFromLeft (12);
    g.setColour (! isActive ? disabled.brighter (0.2f) : textColour != nullptr ? *textColour : colours::text);
    g.setFont (getPopupMenuFont());
    g.drawFittedText (text, r, juce::Justification::centredLeft, 1);
    if (shortcut.isNotEmpty())
    {
        g.setColour (faint);
        g.setFont (ui::font (type::meta));
        g.drawText (shortcut, r, juce::Justification::centredRight);
    }
    if (hasSubMenu)
    {
        juce::Path arrow;
        const float x = (float) r.getRight() - 4.0f, y = (float) area.getCentreY();
        arrow.startNewSubPath (x - 3.0f, y - 3.5f);
        arrow.lineTo (x, y);
        arrow.lineTo (x - 3.0f, y + 3.5f);
        g.setColour (faint);
        g.strokePath (arrow, juce::PathStrokeType (1.3f));
    }
}

void ContextLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& name)
{
    g.setColour (colours::faint);
    g.setFont (labelFont());
    g.drawText (name, area.reduced (12, 0).withTrimmedTop (4), juce::Justification::centredLeft);
}

void ContextLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h)
{
    g.setColour (colours::raised);
    g.fillRoundedRectangle (0.0f, 0.0f, (float) w, (float) h, radius::control);
    g.setColour (colours::border);
    g.drawRoundedRectangle (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f, radius::control, 1.0f);
    juce::AttributedString s;
    s.setJustification (juce::Justification::centredLeft);
    s.append (text, ui::font (12.5f), colours::text);
    juce::TextLayout tl;
    tl.createLayoutWithBalancedLineLengths (s, (float) w - 16.0f);
    tl.draw (g, { 8.0f, 0.0f, (float) w - 16.0f, (float) h });
}

juce::Rectangle<int> ContextLookAndFeel::getTooltipBounds (const juce::String& tip, juce::Point<int> pos, juce::Rectangle<int> parent)
{
    juce::AttributedString s;
    s.append (tip, ui::font (12.5f));
    juce::TextLayout tl;
    tl.createLayoutWithBalancedLineLengths (s, 260.0f);
    const int w = (int) std::ceil (tl.getWidth()) + 16, h = (int) std::ceil (tl.getHeight()) + 10;
    return juce::Rectangle<int> (pos.x > parent.getCentreX() ? pos.x - (w + 12) : pos.x + 12,
                                 pos.y > parent.getCentreY() ? pos.y - (h + 8) : pos.y + 14, w, h).constrainedWithin (parent);
}

// ---------------------------------------------------------------------------
// Sliders (rotary knobs, mini faders)
// ---------------------------------------------------------------------------

void ContextLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end, juce::Slider& s)
{
    using namespace colours;
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (2.0f);
    const float diameter = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto c = bounds.getCentre();
    const float radiusOuter = diameter * 0.5f;
    const bool enabled = s.isEnabled();
    const bool over = s.isMouseOverOrDragging();
    // While dragging the arc follows the mouse exactly; jumps (double-click reset, typed
    // values, undo) glide there.
    static const juce::Identifier posKey ("mPos");
    pos = motion::follow (s, posKey, pos, s.isMouseButtonDown() ? 0.0f : motion::duration::slide, motion::Ease::OutQuint);
    const float hvAmt = motion::hoverAmount (s, enabled && over);
    const float angle = start + pos * (end - start);
    const float trackW = juce::jmax (2.0f, diameter * 0.075f);
    const float arcR = radiusOuter - trackW * 0.5f;

    // Track
    juce::Path track;
    track.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, start, end, true);
    g.setColour (enabled ? border : line);
    g.strokePath (track, juce::PathStrokeType (trackW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Value arc: from the minimum, or from the centre for bipolar controls (gain, pan, tilt).
    const auto range = s.getRange();
    const bool bipolar = range.getStart() < 0.0 && range.getEnd() > 0.0 && std::abs (range.getStart() + range.getEnd()) < 1.0e-6 * range.getLength() + 1.0e-9;
    const float from = bipolar ? start + (float) s.valueToProportionOfLength (0.0) * (end - start) : start;
    if (std::abs (angle - from) > 0.001f)
    {
        juce::Path value;
        value.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
        g.setColour (enabled ? accent.interpolatedWith (accentHi, hvAmt) : disabled.brighter (0.15f));
        g.strokePath (value, juce::PathStrokeType (trackW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Body + pointer
    const float bodyR = arcR - trackW * 1.4f;
    g.setColour (enabled ? raised.interpolatedWith (hover, hvAmt) : panel);
    g.fillEllipse (c.x - bodyR, c.y - bodyR, bodyR * 2.0f, bodyR * 2.0f);
    g.setColour (enabled ? border.brighter (0.15f) : line);
    g.drawEllipse (c.x - bodyR + 0.5f, c.y - bodyR + 0.5f, bodyR * 2.0f - 1.0f, bodyR * 2.0f - 1.0f, 1.0f);
    juce::Path pointer;
    const float pl = bodyR * 0.62f;
    pointer.startNewSubPath (c.x + std::sin (angle) * bodyR * 0.18f, c.y - std::cos (angle) * bodyR * 0.18f);
    pointer.lineTo (c.x + std::sin (angle) * pl, c.y - std::cos (angle) * pl);
    g.setColour (enabled ? text : disabled.brighter (0.3f));
    g.strokePath (pointer, juce::PathStrokeType (juce::jmax (1.5f, diameter * 0.045f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void ContextLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float sliderPos, float, float,
                                           juce::Slider::SliderStyle style, juce::Slider& s)
{
    using namespace colours;
    if (style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, sliderPos, 0, 0, style, s);
        return;
    }
    const float cy = (float) y + (float) h * 0.5f;
    const auto track = juce::Rectangle<float> ((float) x, cy - 1.5f, (float) w, 3.0f);
    static const juce::Identifier posKey ("mPos");
    sliderPos = motion::follow (s, posKey, sliderPos, s.isMouseButtonDown() ? 0.0f : motion::duration::slide, motion::Ease::OutQuint);
    const float hvAmt = motion::hoverAmount (s, s.isEnabled() && s.isMouseOverOrDragging());
    g.setColour (border);
    g.fillRoundedRectangle (track, 1.5f);
    const float zero = s.getRange().contains (0.0) ? (float) x + (float) s.valueToProportionOfLength (0.0) * (float) w : (float) x;
    g.setColour (s.isEnabled() ? accent.withAlpha (0.8f + 0.2f * hvAmt) : disabled);
    g.fillRoundedRectangle (juce::Rectangle<float>::leftTopRightBottom (juce::jmin (zero, sliderPos), cy - 1.5f, juce::jmax (zero, sliderPos), cy + 1.5f), 1.5f);
    g.setColour (s.isEnabled() ? text : disabled.brighter (0.3f));
    const float thumb = 4.0f + 1.0f * hvAmt;
    g.fillEllipse (sliderPos - thumb, cy - thumb, thumb * 2.0f, thumb * 2.0f);
}

juce::Label* ContextLookAndFeel::createSliderTextBox (juce::Slider& s)
{
    auto* l = LookAndFeel_V4::createSliderTextBox (s);
    l->setFont (numberFont (type::meta));
    l->setColour (juce::Label::textColourId, colours::dim);
    l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    l->setColour (juce::TextEditor::backgroundColourId, colours::well);
    l->setColour (juce::TextEditor::textColourId, colours::text);
    l->setColour (juce::TextEditor::outlineColourId, colours::accent.withAlpha (0.6f));
    return l;
}

void ContextLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                                        int thumbStart, int thumbSize, bool over, bool down)
{
    juce::Rectangle<float> thumb = vertical ? juce::Rectangle<float> ((float) x + 2.0f, (float) thumbStart, (float) w - 4.0f, (float) thumbSize)
                                            : juce::Rectangle<float> ((float) thumbStart, (float) y + 2.0f, (float) thumbSize, (float) h - 4.0f);
    g.setColour ((over || down) ? colours::faint : colours::border);
    g.fillRoundedRectangle (thumb, 2.0f);
}

void ContextLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& label)
{
    g.fillAll (label.findColour (juce::Label::backgroundColourId));
    if (label.isBeingEdited())
    {
        g.setColour (colours::accent.withAlpha (0.6f));
        g.drawRect (label.getLocalBounds());
        return;
    }
    const float alpha = label.isEnabled() ? 1.0f : 0.5f;
    const auto f = getLabelFont (label);
    g.setColour (label.findColour (juce::Label::textColourId).withMultipliedAlpha (alpha));
    g.setFont (f);
    const auto area = getLabelBorderSize (label).subtractedFrom (label.getLocalBounds());
    g.drawFittedText (label.getText(), area, label.getJustificationType(),
                      juce::jmax (1, (int) ((float) area.getHeight() / f.getHeight())), label.getMinimumHorizontalScale());
    // Slider value boxes and plain labels: no outline box (a "SaaS form" look we avoid).
}

juce::Font ContextLookAndFeel::getAlertWindowMessageFont() { return ui::font (type::body); }
juce::Font ContextLookAndFeel::getAlertWindowTitleFont()   { return ui::font (type::title, Weight::SemiBold); }
juce::Font ContextLookAndFeel::getLabelFont (juce::Label& l)
{
    return l.getFont().getTypefacePtr() != nullptr ? l.getFont() : ui::font (type::body);
}
} // namespace context::ui
