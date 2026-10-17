#pragma once

#include "Controls.h"

namespace context::ui
{
/** Small toolbar that appears above a selection on the sound lane: the actions that make
    sense for a range, one click away (the same ones are in the right-click menu). */
class SelectionBar : public juce::Component
{
public:
    SelectionBar();

    void showFor (juce::Rectangle<int> anchorInParent, const juce::String& rangeText, bool canGenerate);
    void hide();
    int preferredWidth() const;

    std::function<void()> onNewTake, onFromTake, onSplit, onDelete;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::String range;
    juce::TextButton newTake { "New take" }, fromTake { "From take" }, split { "Split" }, remove { "Delete" };
    motion::Value shown { 0.0f };
    juce::Rectangle<int> target;
};
} // namespace context::ui
