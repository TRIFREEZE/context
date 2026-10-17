#include "KeepPassBar.h"
#include "Generation/Taste.h"

namespace context::ui
{
namespace
{
    // The card is a 40 px pill; buttons are 28 px pills inset 6 px all round, so they sit
    // concentric with it. Text starts 16 px in; groups are 6 px apart.
    constexpr int barH = 40, btnH = 28, inset = (barH - btnH) / 2, gap = 6, textInset = 16;
    int textW (const juce::Font& f, const juce::String& s) { return juce::GlyphArrangement::getStringWidthInt (f, s); }
    juce::Font titleFont()  { return font (type::body + 0.5f, Weight::SemiBold); }
    juce::Font detailFont() { return font (type::body - 0.5f); }
    juce::Font buttonFont() { return font (type::body, Weight::SemiBold); }
}

// -------------------------------------------------------------------- PillButton

PillButton::PillButton (const juce::String& text, Kind k, std::optional<IconButton::Icon> i)
    : juce::Button (text), kind (k), icon (i)
{
    if (kind == Kind::Toggle)
        setClickingTogglesState (true);
}

int PillButton::idealWidth() const
{
    return textW (buttonFont(), getButtonText()) + (icon ? 18 : 0) + 28;
}

void PillButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    const auto r = getLocalBounds().toFloat();
    const float rad = r.getHeight() * 0.5f;
    const float hv = motion::hoverAmount (*this, over && isEnabled());
    const float pr = motion::pressAmount (*this, down);
    const bool on = getToggleState();
    juce::Colour fill, edge, ink;
    switch (kind)
    {
        case Kind::Primary:   fill = colours::accent.brighter (0.08f * hv).darker (0.12f * pr); edge = fill; ink = colours::background; break;
        case Kind::Secondary: fill = colours::raised.interpolatedWith (colours::hover, hv); edge = colours::border; ink = colours::text; break;
        case Kind::Ghost:     fill = colours::hover.withAlpha (0.8f * hv); edge = juce::Colours::transparentBlack; ink = colours::dim.interpolatedWith (colours::text, hv); break;
        case Kind::Toggle:
            fill = on ? colours::accent.withAlpha (0.18f + 0.06f * hv) : colours::raised.interpolatedWith (colours::hover, hv);
            edge = on ? colours::accent.withAlpha (0.8f) : colours::border;
            ink = on ? colours::accent.brighter (0.25f) : colours::text;
            break;
    }
    if (! isEnabled()) { fill = fill.withMultipliedAlpha (0.45f); ink = ink.withMultipliedAlpha (0.45f); }
    const auto body = r.reduced (0.5f).translated (0.0f, 0.6f * pr);
    g.setColour (fill);
    g.fillRoundedRectangle (body, rad);
    if (! edge.isTransparent())
    {
        g.setColour (edge);
        g.drawRoundedRectangle (body.reduced (0.5f), rad - 0.5f, 1.0f);
    }
    // Icon + text, centred together.
    const auto f = buttonFont();
    const float tw = (float) textW (f, getButtonText());
    const float iconW = icon ? 11.0f : 0.0f, iconGap = icon ? 7.0f : 0.0f;
    float x = body.getCentreX() - (iconW + iconGap + tw) * 0.5f;
    if (icon)
    {
        IconButton::drawIcon (g, *icon, juce::Rectangle<float> (x, body.getCentreY() - 5.5f, 11.0f, 11.0f),
                              kind == Kind::Primary ? ink : ink.withMultipliedAlpha (0.85f));
        x += iconW + iconGap;
    }
    g.setColour (ink);
    g.setFont (f);
    g.drawText (getButtonText(), juce::Rectangle<float> (x, body.getY(), tw + 2.0f, body.getHeight()), juce::Justification::centredLeft, false);
}

// -------------------------------------------------------------------- KeepPassBar

KeepPassBar::KeepPassBar()
{
    keep.setTooltip ("Keep this take: it is marked as kept and saved to Favorites");
    pass.setTooltip ("Not this one: say what was off, and the take leaves the lane");
    justPass.setTooltip ("Pass without a reason");
    confirmPass.setTooltip ("Pass with the reasons you picked: the next takes of this prompt avoid them");
    newTake.setTooltip ("Generate another take of this prompt, steered by what you passed on");

    keep.onClick = [this] { if (onKeep) onKeep(); };
    pass.onClick = [this] { setState (State::Reasons); };
    auto commitPass = [this]
    {
        juce::StringArray picked;
        for (int i = 0; i < reasons.size(); ++i)
            if (reasons[i]->getToggleState())
                picked.add (reasonIds[i]);
        if (onPass) onPass (picked);
    };
    confirmPass.onClick = commitPass;
    justPass.onClick = [this] { for (auto* r : reasons) r->setToggleState (false, juce::dontSendNotification); if (onPass) onPass ({}); };
    newTake.onClick = [this] { leave (0.0f); if (onNewTake) onNewTake(); };

    for (auto& r : taste::reasons())
    {
        auto* b = reasons.add (new PillButton (r.label, PillButton::Kind::Toggle));
        b->setTooltip (juce::String ("Next takes: ") + r.steer);
        b->onClick = [this] { repaint(); };
        reasonIds.add (r.id);
        addChildComponent (b);
    }
    for (auto* b : { &keep, &pass, &confirmPass, &justPass, &newTake })
        addChildComponent (b);

    shown.onChange = [this]
    {
        const float s = shown.get();
        setAlpha (juce::jlimit (0.0f, 1.0f, s));
        // Rises 8 px and grows from 96 % as it appears.
        const float sc = 0.96f + 0.04f * s;
        const auto c = getLocalBounds().toFloat().getCentre();
        setTransform (juce::AffineTransform::scale (sc, sc, c.x, c.y).translated (0.0f, (1.0f - s) * 8.0f));
        if (s <= 0.001f && shown.target() <= 0.0f)
            setVisible (false);
    };
    check.onChange = [this] { repaint(); };
    ticker.fn = [this] { timerTick(); };
    setInterceptsMouseClicks (true, true);
    setVisible (false);
}

juce::Button* KeepPassBar::reasonButton (const juce::String& id)
{
    const int i = reasonIds.indexOf (id);
    return i >= 0 ? reasons[i] : nullptr;
}

void KeepPassBar::ask (const juce::String& g, const juce::String& t, const juce::String& l)
{
    if (g == genId && isAsking())
        return;
    genId = g;
    take = t;
    label = l;
    for (auto* r : reasons) r->setToggleState (false, juce::dontSendNotification);
    setState (State::Ask);
    appear();
}

void KeepPassBar::askWhy (const juce::String& g, const juce::String& t, const juce::String& l)
{
    ask (g, t, l);
    genId = g; take = t; label = l;
    setState (State::Reasons);
    appear();
}

void KeepPassBar::dismissQuestion()
{
    if (isAsking())
        leave (0.0f);
}

void KeepPassBar::showKept()
{
    doneText = "Kept";
    doneDetail = "Saved to Favorites";
    setState (State::Kept);
    check.snap (0.0f);
    check.set (1.0f, motion::duration::panel * 1.4f, motion::Ease::OutCubic);
    leave (2400.0f);
}

void KeepPassBar::showPassed (const juce::StringArray& steering, bool offerNewTake)
{
    doneText = "Passed";
    doneDetail = steering.isEmpty() ? juce::String ("Take removed") : "Next takes: " + steering.joinIntoString (", ").toLowerCase();
    newTake.setEnabled (offerNewTake);
    setState (State::Passed);
    check.snap (0.0f);
    check.set (1.0f, motion::duration::panel * 1.4f, motion::Ease::OutCubic);
    leave (5200.0f);
}

void KeepPassBar::setState (State s)
{
    state = s;
    keep.setVisible (s == State::Ask);
    pass.setVisible (s == State::Ask);
    for (auto* r : reasons) r->setVisible (s == State::Reasons);
    confirmPass.setVisible (s == State::Reasons);
    justPass.setVisible (s == State::Reasons);
    newTake.setVisible (s == State::Passed);
    resized();
    repaint();
    if (onLayoutChanged) onLayoutChanged();
}

void KeepPassBar::appear()
{
    leaveAtMs = 0.0;
    if (! isVisible())
    {
        setVisible (true);
        shown.snap (0.0f);
        setAlpha (0.0f);
    }
    shown.set (1.0f, motion::duration::panel * 1.3f, motion::Ease::OutBack);
    if (! motion::enabled())
    {
        setAlpha (1.0f);
        setTransform ({});
    }
}

void KeepPassBar::leave (float afterMs)
{
    if (afterMs <= 0.0f)
    {
        leaveAtMs = 0.0;
        ticker.stopTimer();
        state = state == State::Kept || state == State::Passed ? state : State::Hidden;
        shown.set (0.0f, motion::duration::fade, motion::Ease::OutCubic);
        if (! motion::enabled())
            setVisible (false);
        return;
    }
    leaveAtMs = juce::Time::getMillisecondCounterHiRes() + afterMs;
    ticker.startTimerHz (10);
}

void KeepPassBar::timerTick()
{
    if (leaveAtMs <= 0.0)
        return ticker.stopTimer();
    if (hovered || isMouseOverOrDragging (true))
        leaveAtMs = juce::jmax (leaveAtMs, juce::Time::getMillisecondCounterHiRes() + 900.0);   // not while you read it
    else if (juce::Time::getMillisecondCounterHiRes() >= leaveAtMs)
        leave (0.0f);
}

int KeepPassBar::idealWidth() const
{
    const int right = inset;                        // buttons sit `inset` from the right edge
    switch (state)
    {
        case State::Ask:
            return textInset + 6 + 10 + textW (titleFont(), label) + 10 + textW (detailFont(), "Keep it?") + 18
                 + keep.idealWidth() + gap + pass.idealWidth() + right;
        case State::Reasons:
        {
            int w = textInset + textW (titleFont(), "What was off?") + 16;
            for (auto* r : reasons) w += r->idealWidth() - 6 + gap;
            return w + 6 + justPass.idealWidth() + gap + confirmPass.idealWidth() + right;
        }
        case State::Kept:
        case State::Passed:
            return textInset + 14 + 10 + textW (titleFont(), doneText) + 10 + textW (detailFont(), doneDetail) + 18
                 + (state == State::Passed ? newTake.idealWidth() + right : textInset - 4);
        case State::Hidden: break;
    }
    return 200;
}

void KeepPassBar::resized()
{
    auto r = getLocalBounds().withSizeKeepingCentre (getWidth(), btnH).withTrimmedRight (inset);
    auto place = [&] (PillButton& b, int w) { b.setBounds (r.removeFromRight (w)); r.removeFromRight (gap); };
    switch (state)
    {
        case State::Ask:
            place (pass, pass.idealWidth());
            place (keep, keep.idealWidth());
            break;
        case State::Reasons:
            place (confirmPass, confirmPass.idealWidth());
            place (justPass, justPass.idealWidth());
            r.removeFromRight (6);
            for (int i = reasons.size(); --i >= 0;)
                place (*reasons[i], reasons[i]->idealWidth() - 6);
            break;
        case State::Passed:
            place (newTake, newTake.idealWidth());
            break;
        case State::Kept:
        case State::Hidden:
            break;
    }
}

void KeepPassBar::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    // A floating card: soft shadow, panel, hairline border.
    for (int i = 3; i >= 1; --i)
    {
        g.setColour (juce::Colours::black.withAlpha (0.08f * (float) i));
        g.fillRoundedRectangle (r.reduced ((float) (3 - i)).translated (0.0f, (float) i), (float) barH * 0.5f);
    }
    g.setColour (colours::panel);
    g.fillRoundedRectangle (r.reduced (1.0f), (float) barH * 0.5f - 1.0f);
    g.setColour (colours::border);
    g.drawRoundedRectangle (r.reduced (1.5f), (float) barH * 0.5f - 1.5f, 1.0f);

    auto content = getLocalBounds().withTrimmedLeft (textInset).withTrimmedRight (inset);
    switch (state)
    {
        case State::Ask:
        {
            g.setColour (colours::accent);
            g.fillEllipse ((float) content.getX(), (float) getHeight() * 0.5f - 3.0f, 6.0f, 6.0f);
            content.removeFromLeft (16);
            g.setColour (colours::text);
            g.setFont (titleFont());
            g.drawText (label, content.removeFromLeft (textW (titleFont(), label) + 10), juce::Justification::centredLeft, false);
            g.setColour (colours::dim);
            g.setFont (detailFont());
            g.drawText ("Keep it?", content, juce::Justification::centredLeft, false);
            break;
        }
        case State::Reasons:
            g.setColour (colours::text);
            g.setFont (titleFont());
            g.drawText ("What was off?", content, juce::Justification::centredLeft, false);
            break;
        case State::Kept:
        case State::Passed:
        {
            // The mark draws itself in.
            const bool kept = state == State::Kept;
            auto icon = juce::Rectangle<float> ((float) content.getX(), (float) getHeight() * 0.5f - 7.0f, 14.0f, 14.0f);
            {
                juce::Graphics::ScopedSaveState ss (g);
                g.reduceClipRegion (icon.withWidth (icon.getWidth() * check.get() + 1.0f).expanded (2.0f).toNearestInt());
                IconButton::drawIcon (g, kept ? IconButton::Icon::Check : IconButton::Icon::Close, icon,
                                      kept ? colours::accent : colours::dim);
            }
            content.removeFromLeft (24);
            g.setColour (colours::text);
            g.setFont (titleFont());
            g.drawText (doneText, content.removeFromLeft (textW (titleFont(), doneText) + 10), juce::Justification::centredLeft, false);
            g.setColour (colours::faint);
            g.setFont (detailFont());
            if (state == State::Passed)
                content.removeFromRight (newTake.idealWidth() + gap);
            g.drawText (doneDetail, content, juce::Justification::centredLeft, true);
            break;
        }
        case State::Hidden:
            break;
    }
}

} // namespace context::ui
