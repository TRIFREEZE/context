#include "SelectionBar.h"

namespace context::ui
{
SelectionBar::SelectionBar()
{
    setStyle (newTake, ButtonStyle::Primary);
    for (auto* b : { &fromTake, &split, &remove })
        setStyle (*b, ButtonStyle::Ghost);
    newTake.setTooltip ("Generate a new take in the same style and drop it into this range (F)");
    fromTake.setTooltip ("Use the same bars from another take");
    split.setTooltip ("Split at the selection edges (S)");
    remove.setTooltip ("Silence this range (Delete)");
    newTake.onClick = [this] { if (onNewTake) onNewTake(); };
    fromTake.onClick = [this] { if (onFromTake) onFromTake(); };
    split.onClick = [this] { if (onSplit) onSplit(); };
    remove.onClick = [this] { if (onDelete) onDelete(); };
    for (auto* b : { &newTake, &fromTake, &split, &remove })
        addAndMakeVisible (*b);
    setInterceptsMouseClicks (true, true);
    shown.onChange = [this]
    {
        const float s = shown.get();
        setAlpha (juce::jlimit (0.0f, 1.0f, s));
        setBounds (target.translated (0, juce::roundToInt ((1.0f - s) * 6.0f)));
        if (s <= 0.001f && shown.target() <= 0.0f)
            setVisible (false);
    };
}

int SelectionBar::preferredWidth() const
{
    const auto f = font (type::label, Weight::Medium);
    int w = 12 + juce::GlyphArrangement::getStringWidthInt (f, range) + 12;
    for (auto* b : { &newTake, &fromTake, &split, &remove })
        w += juce::GlyphArrangement::getStringWidthInt (font (type::label + 0.5f, Weight::Medium), b->getButtonText()) + 22;
    return w + 6;
}

void SelectionBar::showFor (juce::Rectangle<int> anchor, const juce::String& rangeText, bool canGenerate)
{
    range = rangeText;
    newTake.setEnabled (canGenerate);
    newTake.setTooltip (canGenerate ? "Generate a new take in the same style and drop it into this range (F)"
                                    : "Generation isn't available right now (licence, bridge, or a generation is running)");
    target = anchor;
    if (! isVisible())
    {
        setVisible (true);
        shown.snap (0.0f);
        setAlpha (0.0f);
        setBounds (target.translated (0, 6));
        shown.set (1.0f, motion::duration::hover * 1.6f, motion::Ease::OutCubic);
        if (! motion::enabled())
            setAlpha (1.0f);
    }
    setBounds (target.translated (0, juce::roundToInt ((1.0f - shown.get()) * 6.0f)));
    resized();
    repaint();
}

void SelectionBar::hide()
{
    if (! isVisible())
        return;
    shown.set (0.0f, motion::duration::hover, motion::Ease::OutCubic);
    if (! motion::enabled())
        setVisible (false);
}

void SelectionBar::resized()
{
    auto r = getLocalBounds().reduced (3);
    r.removeFromLeft (12 + juce::GlyphArrangement::getStringWidthInt (font (type::label, Weight::Medium), range) + 8);
    for (auto* b : { &newTake, &fromTake, &split, &remove })
    {
        const int w = juce::GlyphArrangement::getStringWidthInt (font (type::label + 0.5f, Weight::Medium), b->getButtonText()) + 20;
        b->setBounds (r.removeFromLeft (w));
        r.removeFromLeft (2);
    }
}

void SelectionBar::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillRoundedRectangle (r.translated (0.0f, 2.0f), radius::panel);
    g.setColour (colours::panel);
    g.fillRoundedRectangle (r, radius::panel);
    g.setColour (colours::border);
    g.drawRoundedRectangle (r.reduced (0.5f), radius::panel, 1.0f);
    g.setColour (colours::dim);
    g.setFont (font (type::label, Weight::Medium));
    g.drawText (range, getLocalBounds().withTrimmedLeft (12), juce::Justification::centredLeft, false);
}
} // namespace context::ui
