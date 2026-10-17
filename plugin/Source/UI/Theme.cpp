#include "Theme.h"

namespace context::ui
{
ContextLookAndFeel::ContextLookAndFeel()
{
    using namespace colours;
    setColour (juce::ResizableWindow::backgroundColourId, background);
    setColour (juce::DocumentWindow::backgroundColourId, background);
    setColour (juce::TextButton::buttonColourId, raised);
    setColour (juce::TextButton::buttonOnColourId, accentDark);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, text);
    setColour (juce::ToggleButton::textColourId, text);
    setColour (juce::ToggleButton::tickColourId, accent);
    setColour (juce::ToggleButton::tickDisabledColourId, faint);
    setColour (juce::Label::textColourId, text);
    setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff111318));
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.35f));
    setColour (juce::TextEditor::highlightedTextColourId, text);
    setColour (juce::TextEditor::outlineColourId, border);
    setColour (juce::TextEditor::focusedOutlineColourId, accent.withAlpha (0.8f));
    setColour (juce::CaretComponent::caretColourId, accent);
    setColour (juce::ComboBox::backgroundColourId, raised);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::outlineColourId, border);
    setColour (juce::ComboBox::arrowColourId, dim);
    setColour (juce::PopupMenu::backgroundColourId, raised);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accentDark);
    setColour (juce::PopupMenu::highlightedTextColourId, text);
    setColour (juce::ListBox::backgroundColourId, panel);
    setColour (juce::ListBox::outlineColourId, border);
    setColour (juce::ScrollBar::thumbColourId, border.brighter (0.3f));
    setColour (juce::Slider::thumbColourId, accent);
    setColour (juce::Slider::trackColourId, accentDark);
    setColour (juce::Slider::backgroundColourId, raised);
    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff111318));
    setColour (juce::Slider::textBoxOutlineColourId, border);
    setColour (juce::ProgressBar::backgroundColourId, raised);
    setColour (juce::ProgressBar::foregroundColourId, accent);
    setColour (juce::AlertWindow::backgroundColourId, panel);
    setColour (juce::AlertWindow::textColourId, text);
    setColour (juce::AlertWindow::outlineColourId, border);
    setColour (juce::TooltipWindow::backgroundColourId, raised);
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, border);
}

juce::Font ContextLookAndFeel::font (float height, bool bold)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

juce::Font ContextLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return font (juce::jmin (15.0f, (float) buttonHeight * 0.5f), true);
}

void ContextLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    auto c = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : bg;
    if (! b.isEnabled())   c = c.withMultipliedSaturation (0.3f).withMultipliedAlpha (0.45f);
    else if (down)         c = c.darker (0.15f);
    else if (over)         c = c.brighter (0.08f);
    g.setColour (c);
    g.fillRoundedRectangle (r, 6.0f);
    if (b.getToggleState())
    {
        g.setColour (colours::accent.withAlpha (0.9f));
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.2f);
    }
    else if (bg == colours::raised)
    {
        g.setColour (colours::border);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    }
}

void ContextLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    g.setFont (getTextButtonFont (b, b.getHeight()));
    auto c = b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId);
    g.setColour (b.isEnabled() ? c : c.withAlpha (0.4f));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (6, 2), juce::Justification::centred, 1, 0.8f);
}

void ContextLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool)
{
    const auto r = b.getLocalBounds();
    const float boxSize = 16.0f;
    auto box = juce::Rectangle<float> (2.0f, (r.getHeight() - boxSize) * 0.5f, boxSize, boxSize);
    const bool on = b.getToggleState();
    const float alpha = b.isEnabled() ? 1.0f : 0.4f;

    g.setColour ((on ? colours::accent : colours::raised).withAlpha (alpha));
    g.fillRoundedRectangle (box, 4.0f);
    g.setColour ((on ? colours::accent : (over ? colours::dim : colours::border)).withAlpha (alpha));
    g.drawRoundedRectangle (box.reduced (0.5f), 4.0f, 1.0f);
    if (on)
    {
        juce::Path tick;
        tick.startNewSubPath (box.getX() + 4.0f, box.getCentreY());
        tick.lineTo (box.getX() + 7.0f, box.getBottom() - 4.5f);
        tick.lineTo (box.getRight() - 3.5f, box.getY() + 4.5f);
        g.setColour (juce::Colour (0xff0b1312).withAlpha (alpha));
        g.strokePath (tick, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    g.setColour (colours::text.withAlpha (alpha));
    g.setFont (font (14.0f));
    g.drawFittedText (b.getButtonText(), r.withTrimmedLeft ((int) boxSize + 10), juce::Justification::centredLeft, 1, 0.85f);
}

void ContextLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    g.setColour (e.findColour (juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle (juce::Rectangle<float> (0, 0, (float) w, (float) h), 6.0f);
}

void ContextLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    const bool focus = e.hasKeyboardFocus (true) && ! e.isReadOnly();
    g.setColour (e.findColour (focus ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId));
    g.drawRoundedRectangle (juce::Rectangle<float> (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f), 6.0f, focus ? 1.5f : 1.0f);
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

void ContextLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (r, 6.0f, 1.0f);
    juce::Path arrow;
    const float cx = (float) w - 14.0f, cy = (float) h * 0.5f;
    arrow.addTriangle (cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f, cx, cy + 3.0f);
    g.setColour (colours::dim);
    g.fillPath (arrow);
}
} // namespace context::ui
