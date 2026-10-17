#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/** Context's design system.

    One place for colour, type, spacing, radii and control sizes. Components use these
    tokens instead of inventing their own values, which keeps the interface consistent
    and makes later restyling a one-file change.

    Direction: dark neutral greys, information-dense, thin separators instead of boxes,
    small radii, one restrained teal accent for "active / yours / play". */
namespace context::ui
{
namespace colours
{
    // Surfaces (darkest to lightest)
    inline const juce::Colour background { 0xff0d0e10 };   // window
    inline const juce::Colour well       { 0xff0a0b0c };   // waveform wells, text fields
    inline const juce::Colour panel      { 0xff131518 };   // sections / top bar
    inline const juce::Colour raised     { 0xff1b1e22 };   // controls
    inline const juce::Colour hover      { 0xff23272c };   // hovered controls
    inline const juce::Colour pressed    { 0xff2a2f35 };

    // Lines
    inline const juce::Colour border     { 0xff272b31 };   // control outlines
    inline const juce::Colour line       { 0xff1f2226 };   // section separators
    inline const juce::Colour grid       { 0xff1b1e21 };   // bar grid inside wells

    // Text
    inline const juce::Colour text       { 0xffe6e8eb };   // primary
    inline const juce::Colour dim        { 0xffa0a6ae };   // secondary
    inline const juce::Colour faint      { 0xff6a7079 };   // tertiary / labels
    inline const juce::Colour disabled   { 0xff40454c };

    // Accent (used sparingly: selection, play, the output you made)
    inline const juce::Colour accent     { 0xff3ccfc0 };
    inline const juce::Colour accentHi   { 0xff66ddd1 };
    inline const juce::Colour accentDark { 0xff15403c };
    inline const juce::Colour accentText { 0xff051312 };   // text on accent fills

    // Signals
    inline const juce::Colour good       { 0xff5fcb8b };
    inline const juce::Colour warn       { 0xffe8b65c };
    inline const juce::Colour bad        { 0xffe26d6d };
    inline const juce::Colour favorite   { 0xffe8b65c };
    inline const juce::Colour record     { 0xffe25d5d };

    // Waveforms
    inline const juce::Colour waveform   { 0xff3ccfc0 };   // output lane
    inline const juce::Colour contextWave{ 0xff8b96a3 };   // track context lane
}

namespace space
{
    inline constexpr int xs = 4, s = 8, m = 12, l = 16, xl = 24;
}

namespace radius
{
    inline constexpr float small = 2.0f, control = 3.0f, panel = 5.0f;
}

namespace layout
{
    /** Width of the left column (lane headers, inspector side column): everything to its
        right shares the timeline's x-axis. */
    inline constexpr int laneHeader = 176;
}

namespace height
{
    inline constexpr int small = 20, control = 24, large = 30, topBar = 40, transport = 46;
}

enum class Weight { Regular, Medium, SemiBold };

/** Inter (embedded) at the requested size and weight. */
juce::Font font (float size, Weight = Weight::Regular);
/** Tabular figures for numbers that change (BPM, times, values). */
juce::Font numberFont (float size, Weight = Weight::Medium);
/** Small uppercase label style (section names, lane names). */
juce::Font labelFont();

/** Dark, flat, compact look for every JUCE control in the plugin. */
class ContextLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ContextLookAndFeel();

    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool isMouseOverButton, bool isButtonDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool isMouseOverButton, bool isButtonDown) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool isMouseOverButton, bool isButtonDown) override;
    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawProgressBar (juce::Graphics&, juce::ProgressBar&, int width, int height, double progress, const juce::String& text) override;
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText,
                            const juce::Drawable* icon, const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>& area, const juce::String& sectionName) override;
    juce::Font getPopupMenuFont() override;
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth, int& idealHeight) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;
    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPosProportional,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool isScrollbarVertical,
                        int thumbStartPosition, int thumbSize, bool isMouseOver, bool isMouseDown) override;
    int getDefaultScrollbarWidth() override { return 8; }
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowTitleFont() override;
    juce::Font getLabelFont (juce::Label&) override;
    void drawLabel (juce::Graphics&, juce::Label&) override;

    /** Kept for the existing tabs: bold = SemiBold. */
    static juce::Font font (float height, bool bold = false);
};

// ---- small painting helpers (shared by all panels) ----

/** Thin progress bar; progress outside 0..1 draws an indeterminate sliding segment. */
void drawProgress (juce::Graphics& g, juce::Rectangle<float> r, double progress);

/** Section background with an optional hairline border. */
inline void paintCard (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill = colours::panel)
{
    g.setColour (fill);
    g.fillRoundedRectangle (r, radius::panel);
    g.setColour (colours::line);
    g.drawRoundedRectangle (r.reduced (0.5f), radius::panel, 1.0f);
}

/** Small uppercase section label. */
inline void paintHeading (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r, juce::Colour c = colours::faint)
{
    g.setColour (c);
    g.setFont (labelFont());
    g.drawText (text.toUpperCase(), r, juce::Justification::centredLeft, true);
}

inline void paintSeparator (juce::Graphics& g, int y, int x0, int x1)
{
    g.setColour (colours::line);
    g.fillRect (x0, y, x1 - x0, 1);
}

/** Button variants (stored as a component property; the look-and-feel reads it). */
enum class ButtonStyle { Secondary, Primary, Ghost, Toggle, Danger, Tab };
void setStyle (juce::Button&, ButtonStyle);

inline void stylePrimary (juce::TextButton& b) { setStyle (b, ButtonStyle::Primary); }
inline void styleQuiet (juce::TextButton& b)   { setStyle (b, ButtonStyle::Secondary); }

/** UTF-8 helper for the few non-ASCII glyphs we use. */
inline juce::String glyph (const char* utf8) { return juce::String (juce::CharPointer_UTF8 (utf8)); }
} // namespace context::ui
