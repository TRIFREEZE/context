#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Controls.h"
#include "Generation/GenerationModels.h"

namespace context
{
class ContextAudioProcessor;
}

namespace context::ui
{
/** Recent / Favorites / All generations: preview, favorite, drag, reveal, remove. */
class HistoryTab : public juce::Component, private juce::ListBoxModel
{
public:
    explicit HistoryTab (ContextAudioProcessor&);
    ~HistoryTab() override;

    void refresh();
    std::function<void()> onUseResult;      // switch to the Generate tab with this result selected

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum class Filter { Recent, Favorites, All };

    int getNumRows() override;
    void paintListBoxItem (int, juce::Graphics&, int, int, bool) override {}
    juce::Component* refreshComponentForRow (int row, bool selected, juce::Component* existing) override;

    void removeEntry (const GenerationResult&);

    class Row;
    ContextAudioProcessor& processor;
    Filter filter = Filter::Recent;
    std::vector<GenerationResult> items;
    SegmentedControl filterSwitch;
    juce::ListBox list;
    juce::Label emptyLabel, infoLabel;
    juce::Rectangle<int> columnHeader;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HistoryTab)
};
} // namespace context::ui
