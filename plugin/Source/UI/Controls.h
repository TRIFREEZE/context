#pragma once

#include "Theme.h"
#include "Motion.h"
#include "DSP/EffectParameter.h"

namespace context::ui
{
/** Compact segmented selector ("2 4 6 8 16", "Auto Fit Trim Loop Repitch"). */
class SegmentedControl : public juce::Component, public juce::TooltipClient
{
public:
    SegmentedControl() = default;

    void setItems (const juce::StringArray& labels, const juce::StringArray& tooltips = {});
    void setSelected (int index, juce::NotificationType = juce::dontSendNotification);
    int getSelected() const noexcept { return selected; }
    void setItemEnabled (int index, bool enabled);
    void setItemTooltip (int index, const juce::String&);
    /** Segments sized to their text instead of equal widths. */
    void setProportional (bool b) { proportional = b; resized(); repaint(); }
    void setFontSize (float s) { fontSize = s; repaint(); }
    int idealWidth() const;

    std::function<void (int)> onChange;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    juce::String getTooltip() override;

private:
    int indexAt (juce::Point<int>) const;
    void movePill (bool animate);
    juce::StringArray items, tips;
    juce::Array<bool> enabled;
    juce::Array<juce::Rectangle<int>> cells;
    int selected = -1, hovered = -1;
    bool proportional = false;
    float fontSize = type::label;
    motion::Rect pill;          // the selection slides between segments
};

/** Small square lane toggle ("M", "S"). */
class LaneToggle : public juce::Button
{
public:
    LaneToggle (const juce::String& letter, juce::Colour onColour);
    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    juce::Colour on;
};

/** Icon-only button. */
class IconButton : public juce::Button
{
public:
    enum class Icon { Play, Stop, Rewind, Loop, Power, Plus, Folder, Copy, Star, StarFilled, Drag, Record, Close, Save, More, Refresh, Variation,
                      Undo, Redo, Split, Trash, Keys, Sparkle, Takes, Magnet, Help };
    explicit IconButton (Icon, const juce::String& tooltip = {});
    void setIcon (Icon i) { icon = i; repaint(); }
    void setOnColour (juce::Colour c) { onColour = c; repaint(); }
    void paintButton (juce::Graphics&, bool over, bool down) override;
    static void drawIcon (juce::Graphics&, Icon, juce::Rectangle<float>, juce::Colour);

private:
    Icon icon;
    juce::Colour onColour = colours::accent;
};

/** Common base for controls bound to an effect parameter. */
class ParameterControl : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit ParameterControl (fx::EffectParameter& p) : param (p) {}
    /** Pull the value from the parameter (after reset/undo/state load) and dim if irrelevant. */
    virtual void refresh (bool relevant) = 0;
    /** Preferred size in the inspector grid. */
    virtual int preferredWidth() const { return 64; }
    std::function<void()> onEdit;    // after the user changed the value
    std::function<void()> onGestureStart, onGestureEnd;   // a drag: one undo step
    fx::EffectParameter& param;
};

/** Rotary knob with name and value; drag (Shift = fine), double-click resets, double-click
    the value to type one. */
class Knob : public ParameterControl
{
public:
    explicit Knob (fx::EffectParameter&);
    void refresh (bool relevant) override;
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    juce::Slider slider;
    juce::Label value;
    bool syncing = false;
};

/** Choice parameter: segmented when short, dropdown otherwise. */
class ChoiceControl : public ParameterControl
{
public:
    explicit ChoiceControl (fx::EffectParameter&);
    void refresh (bool relevant) override;
    void resized() override;
    void paint (juce::Graphics&) override;
    int preferredWidth() const override;

private:
    juce::ComboBox box;
    SegmentedControl segments;
    bool useSegments = false;
};

/** On/off parameter. */
class ToggleControl : public ParameterControl
{
public:
    explicit ToggleControl (fx::EffectParameter&);
    void refresh (bool relevant) override;
    void resized() override;
    void paint (juce::Graphics&) override;
    int preferredWidth() const override { return 72; }

private:
    juce::TextButton button;
};

std::unique_ptr<ParameterControl> makeParameterControl (fx::EffectParameter&);

/** Truncates text to fit a width, with an ellipsis. */
juce::String fitText (const juce::String&, const juce::Font&, float width);
} // namespace context::ui
