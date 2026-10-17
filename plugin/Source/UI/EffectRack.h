#pragma once

#include "Controls.h"
#include "DSP/EffectChain.h"

namespace context { class ContextAudioProcessor; }

namespace context::ui
{
class EffectRackPanel;

/** One module in the rack row: name, power (bypass), drag to reorder. */
class EffectChip : public juce::Component, public juce::SettableTooltipClient
{
public:
    EffectChip (EffectRackPanel&, int index);
    void update (const juce::String& label, bool bypassed, bool selected);
    int getIndex() const noexcept { return index; }
    /** Slides to `target` (or jumps there the first time). */
    void moveTo (juce::Rectangle<int> target, juce::Rectangle<int> from = {});

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

private:
    juce::Rectangle<int> powerArea() const { return getLocalBounds().removeFromLeft (22); }
    EffectRackPanel& rack;
    int index;
    juce::String label;
    bool bypassed = false, selected = false, dragging = false;
    motion::Rect place;
};

/** EQ response curve with band handles (click a handle to edit that band). */
class EqCurve : public juce::Component
{
public:
    explicit EqCurve (fx::EffectProcessor& eq) : effect (eq) {}
    std::function<void (int band)> onBandClicked;
    int selectedBand = 0;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::Point<float> handleFor (int band) const;
    fx::EffectProcessor& effect;
};

/** PROCESSING: the rack row and the selected effect's inspector. */
class EffectRackPanel : public juce::Component
{
public:
    explicit EffectRackPanel (ContextAudioProcessor&);
    ~EffectRackPanel() override;

    void refresh();
    void tick();

    void paint (juce::Graphics&) override;
    void resized() override;

    // chip callbacks
    void chipClicked (int index);
    void chipPowerClicked (int index);
    void chipMenu (int index);
    void chipDragged (int index, juce::Point<int> posInRack);
    void chipDropped (int index, juce::Point<int> posInRack);

private:
    void rebuildChips();
    void rebuildInspector();
    void layoutInspector();
    void showAddMenu();
    void edited();
    int insertionIndexFor (juce::Point<int>) const;
    juce::String chainSignature() const;

    ContextAudioProcessor& processor;
    fx::EffectChain& chain;

    juce::OwnedArray<EffectChip> chips;
    juce::TextButton addButton { "+ Add effect" };
    SegmentedControl abSwitch;
    juce::String selectedId, signature, inspectorFor;

    // inspector
    juce::OwnedArray<ParameterControl> controls;
    std::unique_ptr<SegmentedControl> bandSelector;
    std::unique_ptr<EqCurve> eqCurve;
    juce::TextButton bypassButton { "Bypass" }, resetButton { "Reset" }, removeButton { "Remove" };
    int eqBand = 2;

    juce::Rectangle<int> headerArea, rowArea, inspectorArea, inspectorTitle, meterArea, emptyArea;
    int dragInsert = -1;
    juce::Point<int> dragPos;
    int draggingIndex = -1;
    float meterDb = 0.0f;
    std::map<juce::String, juce::Rectangle<int>> lastChipBounds;   // by instance id, for slide animations
    motion::Value inspectorFade { 1.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EffectRackPanel)
};
} // namespace context::ui
