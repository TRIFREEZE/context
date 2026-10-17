#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/** Context's design system.

    One place for colour, type, spacing, radii and control sizes. Components use these
    tokens instead of inventing their own values, which keeps the interface consistent
    and makes later restyling a one-file change.

    Direction: a studio instrument, not a web page. Graphite surfaces with real depth
    (wells are darker than the panel, controls are lighter), Barlow (a DIN-style grotesk
    from road and hardware lettering) in sentence case, hairlines instead of boxes, and
    colour that means something:
        silver  = your track (the captured context)
        teal    = the sound Context makes, and every "go" action
        amber   = a lamp: recording / armed / attention
    Motion answers what the user does (see Motion.h); nothing moves on its own except the
    playhead and progress. */
namespace context::ui
{
namespace colours
{
    // Surfaces (darkest to lightest)
    inline const juce::Colour well       { 0xff111316 };   // waveform wells, text fields
    inline const juce::Colour background { 0xff17191c };   // window
    inline const juce::Colour panel      { 0xff1c1f23 };   // top bar, prompt, sheets
    inline const juce::Colour raised     { 0xff24282d };   // controls
    inline const juce::Colour hover      { 0xff2c3137 };   // hovered controls
    inline const juce::Colour pressed    { 0xff343a41 };

    // Lines
    inline const juce::Colour border     { 0xff30353b };   // control outlines
    inline const juce::Colour line       { 0xff25292e };   // section separators
    inline const juce::Colour grid       { 0xff1d2024 };   // bar grid inside wells

    // Text
    inline const juce::Colour text       { 0xffe9ebee };   // primary
    inline const juce::Colour dim        { 0xffa3a9b1 };   // secondary
    inline const juce::Colour faint      { 0xff707780 };   // tertiary / labels
    inline const juce::Colour disabled   { 0xff464b52 };

    // Teal: the sound you make, selection, play
    inline const juce::Colour accent     { 0xff43c7b8 };
    inline const juce::Colour accentHi   { 0xff6fd8cc };
    inline const juce::Colour accentDark { 0xff17423e };
    inline const juce::Colour accentText { 0xff06201d };   // text on accent fills

    // Amber lamp: recording, armed, attention
    inline const juce::Colour lamp       { 0xffe6a04a };
    inline const juce::Colour lampDark   { 0xff4a3519 };

    // Signals
    inline const juce::Colour good       { 0xff63c98d };
    inline const juce::Colour warn       { 0xffe6a04a };
    inline const juce::Colour bad        { 0xffe0706b };
    inline const juce::Colour favorite   { 0xffe6a04a };
    inline const juce::Colour record     { 0xffe0574f };

    // Waveforms
    inline const juce::Colour waveform   { 0xff43c7b8 };   // output lane
    inline const juce::Colour contextWave{ 0xff9aa7b4 };   // track context lane (silver)
    inline const juce::Colour selection  { 0x3343c7b8 };   // selected range on a lane

    // Overlay behind sheets
    inline const juce::Colour scrim      { 0xa0090a0c };

    /** Distinct but quiet colours for takes (take 1, 2, 3...). */
    juce::Colour forTake (int number);
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
    inline constexpr int small = 20, control = 24, large = 30, topBar = 42, transport = 48;
}

/** Type scale (Barlow): meta 11.5, label 12, body 13, title 15, readout 20. */
namespace type
{
    inline constexpr float meta = 11.5f, label = 12.0f, body = 13.0f, title = 15.0f, readout = 20.0f;
}

enum class Weight { Regular, Medium, SemiBold };

/** Barlow (embedded) at the requested size and weight. */
juce::Font font (float size, Weight = Weight::Regular);
/** Tabular figures for numbers that change (BPM, times, values). */
juce::Font numberFont (float size, Weight = Weight::Medium);
/** Section / lane names: sentence case, medium weight, small. */
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
    juce::PopupMenu::Options getOptionsForComboBoxPopupMenu (juce::ComboBox&, juce::Label&) override;
    int getPopupMenuBorderSize() override { return 5; }
    void drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics&, const juce::Path&, juce::Image&) override;

    /** Kept for the existing tabs: bold = SemiBold. */
    static juce::Font font (float height, bool bold = false);
};

// ---- small painting helpers (shared by all panels) ----

/** Thin progress bar; progress outside 0..1 draws an indeterminate sliding segment. */
void drawProgress (juce::Graphics& g, juce::Rectangle<float> r, double progress);

/** Every menu goes through here: it gets Context's look (JUCE otherwise falls back to its stock
    look for menus) and opens inside the plugin window, with rounded corners. */
void showMenu (juce::PopupMenu& menu, juce::Component& owner, juce::PopupMenu::Options options, std::function<void (int)> onResult);

/** Section background with an optional hairline border. */
inline void paintCard (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill = colours::panel)
{
    g.setColour (fill);
    g.fillRoundedRectangle (r, radius::panel);
    g.setColour (colours::line);
    g.drawRoundedRectangle (r.reduced (0.5f), radius::panel, 1.0f);
}

/** Section label (sentence case: "Track context", "Processing"). */
inline void paintHeading (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r, juce::Colour c = colours::dim)
{
    g.setColour (c);
    g.setFont (labelFont());
    g.drawText (text, r, juce::Justification::centredLeft, true);
}

inline void paintSeparator (juce::Graphics& g, int y, int x0, int x1)
{
    g.setColour (colours::line);
    g.fillRect (x0, y, x1 - x0, 1);
}

/** Button variants (stored as a component property; the look-and-feel reads it). */
enum class ButtonStyle { Secondary, Primary, Ghost, Toggle, Danger, Tab, Nav };
void setStyle (juce::Button&, ButtonStyle);

inline void stylePrimary (juce::TextButton& b) { setStyle (b, ButtonStyle::Primary); }
inline void styleQuiet (juce::TextButton& b)   { setStyle (b, ButtonStyle::Secondary); }

/** UTF-8 helper for the few non-ASCII glyphs we use. */
inline juce::String glyph (const char* utf8) { return juce::String (juce::CharPointer_UTF8 (utf8)); }
} // namespace context::ui
