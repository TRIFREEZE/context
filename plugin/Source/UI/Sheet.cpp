#include "Sheet.h"

namespace context::ui
{
namespace
{
    constexpr int panelWidth = 440, pad = 22;
}

bool Sheet::show (juce::Component& from, const juce::String& title, const juce::String& body,
                  std::vector<Action> actions, std::unique_ptr<juce::Component> extra, int extraHeight)
{
    auto* host = dynamic_cast<SheetHost*> (&from);
    for (auto* c = from.getParentComponent(); host == nullptr && c != nullptr; c = c->getParentComponent())
        host = dynamic_cast<SheetHost*> (c);
    if (host == nullptr)
        return false;
    if (actions.empty())
        actions.push_back ({ "Close", ButtonStyle::Secondary, {} });
    auto sheet = std::make_unique<Sheet> (title, body, std::move (actions), std::move (extra), extraHeight);
    sheet->host = host;
    host->presentSheet (std::move (sheet));
    return true;
}

Sheet::Sheet (const juce::String& t, const juce::String& b, std::vector<Action> a, std::unique_ptr<juce::Component> x, int xh)
    : title (t), body (b), actions (std::move (a)), extra (std::move (x)), extraHeight (xh)
{
    setWantsKeyboardFocus (true);
    setAlwaysOnTop (true);
    for (int i = 0; i < (int) actions.size(); ++i)
    {
        auto* btn = buttons.add (new juce::TextButton (actions[(size_t) i].text));
        setStyle (*btn, actions[(size_t) i].style);
        btn->onClick = [this, i] { close (i); };
        addAndMakeVisible (btn);
    }
    if (extra != nullptr)
        addAndMakeVisible (*extra);
    shown.onChange = [this]
    {
        setAlpha (juce::jlimit (0.0f, 1.0f, shown.get()));
        resized();
        repaint();
    };
}

void Sheet::appear()
{
    shown.snap (0.0f);
    setAlpha (0.0f);
    shown.set (1.0f, motion::duration::panel, motion::Ease::OutQuint);
    if (! motion::enabled())
        setAlpha (1.0f);
    grabKeyboardFocus();
    // The primary action (last non-cancel style) gets Enter.
    for (int i = (int) buttons.size(); --i >= 0;)
        if (actions[(size_t) i].style == ButtonStyle::Primary || actions[(size_t) i].style == ButtonStyle::Danger)
        {
            buttons[i]->setWantsKeyboardFocus (true);
            break;
        }
}

void Sheet::close (int index)
{
    if (closing)
        return;
    closing = true;
    // A cancel (Esc / outside click) runs the action whose style is not Primary/Danger, if any.
    std::function<void()> run;
    if (juce::isPositiveAndBelow (index, (int) actions.size()))
        run = actions[(size_t) index].run;
    for (auto* b : buttons) b->setEnabled (false);
    auto* h = host;
    juce::Component::SafePointer<Sheet> safe (this);
    auto finish = [safe, h, run]
    {
        if (run) run();
        if (safe != nullptr && h != nullptr)
            h->dismissSheet (safe.getComponent());
    };
    if (! motion::enabled())
    {
        juce::MessageManager::callAsync (finish);
        return;
    }
    shown.onChange = [this, finish]
    {
        setAlpha (juce::jlimit (0.0f, 1.0f, shown.get()));
        resized();
        if (shown.get() <= 0.001f)
        {
            shown.onChange = nullptr;
            juce::MessageManager::callAsync (finish);
        }
    };
    shown.set (0.0f, motion::duration::fade, motion::Ease::OutCubic);
}

int Sheet::bodyHeight (int width) const
{
    if (body.isEmpty())
        return 0;
    juce::AttributedString s;
    s.append (body, font (type::body), colours::dim);
    s.setLineSpacing (3.0f);
    juce::TextLayout tl;
    tl.createLayout (s, (float) width);
    return (int) std::ceil (tl.getHeight());
}

juce::Rectangle<int> Sheet::panelBounds() const
{
    const int w = juce::jmin (panelWidth + (extra != nullptr ? 120 : 0), getWidth() - 40);
    const int inner = w - pad * 2;
    const int h = pad + 24 + 8 + bodyHeight (inner) + (extra != nullptr ? extraHeight + 14 : 0) + 22 + height::large + pad;
    // Sheets settle 10 px upwards as they appear.
    const int lift = juce::roundToInt ((1.0f - shown.get()) * 10.0f);
    return juce::Rectangle<int> (w, h).withCentre (getLocalBounds().getCentre().translated (0, -getHeight() / 12 + lift));
}

void Sheet::resized()
{
    auto p = panelBounds().reduced (pad);
    auto row = p.removeFromBottom (height::large);
    for (int i = buttons.size(); --i >= 0;)
    {
        auto* b = buttons[i];
        const int w = juce::jmax (88, juce::GlyphArrangement::getStringWidthInt (font (type::body, Weight::Medium), b->getButtonText()) + 32);
        b->setBounds (row.removeFromRight (w));
        row.removeFromRight (space::s);
    }
    if (extra != nullptr)
    {
        p.removeFromBottom (22);
        extra->setBounds (p.removeFromBottom (extraHeight));
    }
}

void Sheet::paint (juce::Graphics& g)
{
    g.fillAll (colours::scrim);
    const auto panel = panelBounds().toFloat();
    // soft shadow
    for (int i = 0; i < 6; ++i)
    {
        g.setColour (juce::Colours::black.withAlpha (0.05f));
        g.fillRoundedRectangle (panel.expanded ((float) i * 2.0f).translated (0.0f, 4.0f), radius::panel + (float) i * 2.0f);
    }
    g.setColour (colours::panel);
    g.fillRoundedRectangle (panel, radius::panel + 1.0f);
    g.setColour (colours::border);
    g.drawRoundedRectangle (panel.reduced (0.5f), radius::panel + 1.0f, 1.0f);

    auto r = panel.toNearestInt().reduced (pad);
    g.setColour (colours::text);
    g.setFont (font (type::title + 1.0f, Weight::SemiBold));
    g.drawText (title, r.removeFromTop (24), juce::Justification::centredLeft, true);
    r.removeFromTop (8);
    if (body.isNotEmpty())
    {
        juce::AttributedString s;
        s.append (body, font (type::body), colours::dim);
        s.setLineSpacing (3.0f);
        juce::TextLayout tl;
        tl.createLayout (s, (float) r.getWidth());
        tl.draw (g, r.removeFromTop ((int) std::ceil (tl.getHeight())).toFloat());
    }
}

void Sheet::mouseUp (const juce::MouseEvent& e)
{
    if (! panelBounds().contains (e.getPosition()))
        close (-1);
}

bool Sheet::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        close (-1);
        return true;
    }
    if (k == juce::KeyPress::returnKey)
    {
        for (int i = (int) actions.size(); --i >= 0;)
            if (actions[(size_t) i].style == ButtonStyle::Primary || actions[(size_t) i].style == ButtonStyle::Danger)
            {
                close (i);
                return true;
            }
    }
    return true;   // a sheet is modal: keys don't reach the workspace underneath
}
} // namespace context::ui
