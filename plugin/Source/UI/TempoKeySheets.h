#pragma once

#include "Theme.h"
#include "Motion.h"

namespace context { class ContextAudioProcessor; }

namespace context::ui
{
/** A value you can click to change (the BPM and key in "Your track"): plain text that gets a
    soft pill and a chevron when hovered, so it reads as a value first and a control second. */
class ValueChip : public juce::Component, public juce::SettableTooltipClient
{
public:
    void setText (const juce::String& text, juce::Font font, juce::Colour colour);
    /** A small unit after the value ("BPM"), inside the chip. */
    void setUnit (const juce::String& unit);
    int idealWidth() const;
    std::function<void()> onClick;

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::String text, unit;
    juce::Font textFont { juce::FontOptions {} };
    int unitWidth() const;
    juce::Colour colour;
    motion::Value change { 1.0f };      // 0 -> 1 when the value changes: it settles into place
};

/** "Tempo" sheet: type a tempo, or pick half-time / double-time / the DAW's tempo. */
void showTempoSheet (juce::Component& from, ContextAudioProcessor&);

/** "Key" sheet: pick the root and major/minor, or go back to detecting it. */
void showKeySheet (juce::Component& from, ContextAudioProcessor&);
} // namespace context::ui
