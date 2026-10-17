#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace context::ui
{
namespace colours
{
    inline const juce::Colour background { 0xff0e0f12 };
    inline const juce::Colour panel      { 0xff16181d };
    inline const juce::Colour raised     { 0xff1e2128 };
    inline const juce::Colour border     { 0xff272b33 };
    inline const juce::Colour text       { 0xffeceef2 };
    inline const juce::Colour dim        { 0xff8b919c };
    inline const juce::Colour faint      { 0xff5a606b };
    inline const juce::Colour accent     { 0xff4fd1c5 };   // primary action
    inline const juce::Colour accentDark { 0xff1f5f59 };
    inline const juce::Colour good       { 0xff4ade80 };
    inline const juce::Colour warn       { 0xfff6c453 };
    inline const juce::Colour bad        { 0xffff6b6b };
    inline const juce::Colour favorite   { 0xfff6c453 };
    inline const juce::Colour waveform   { 0xff7fe3d8 };
}

/** Dark, flat, producer-oriented look. */
class ContextLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ContextLookAndFeel();

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool isMouseOverButton, bool isButtonDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool isMouseOverButton, bool isButtonDown) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool isMouseOverButton, bool isButtonDown) override;
    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawProgressBar (juce::Graphics&, juce::ProgressBar&, int width, int height, double progress, const juce::String& text) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;

    static juce::Font font (float height, bool bold = false);
};

/** Paints a rounded card background. */
inline void paintCard (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill = colours::panel)
{
    g.setColour (fill);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (colours::border);
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
}

/** Thin progress bar; progress outside 0..1 draws an indeterminate sliding segment. */
void drawProgress (juce::Graphics& g, juce::Rectangle<float> r, double progress);

/** Small uppercase section heading. */
inline void paintHeading (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r)
{
    g.setColour (colours::dim);
    g.setFont (ContextLookAndFeel::font (11.5f, true).withExtraKerningFactor (0.12f));
    g.drawText (text.toUpperCase(), r, juce::Justification::centredLeft);
}

/** Makes a TextButton look like a primary (accent) action. */
inline void stylePrimary (juce::TextButton& b)
{
    b.setColour (juce::TextButton::buttonColourId, colours::accent);
    b.setColour (juce::TextButton::buttonOnColourId, colours::accent);
    b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff0b1312));
    b.setColour (juce::TextButton::textColourOnId, juce::Colour (0xff0b1312));
}

inline void styleQuiet (juce::TextButton& b)
{
    b.setColour (juce::TextButton::buttonColourId, colours::raised);
    b.setColour (juce::TextButton::buttonOnColourId, colours::accentDark);
    b.setColour (juce::TextButton::textColourOffId, colours::text);
    b.setColour (juce::TextButton::textColourOnId, colours::text);
}

/** UTF-8 helper for the few non-ASCII glyphs we use. */
inline juce::String glyph (const char* utf8) { return juce::String (juce::CharPointer_UTF8 (utf8)); }
} // namespace context::ui
