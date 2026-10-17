#pragma once

#include "Controls.h"
#include "Generation/GenerationModels.h"

namespace context { class ContextAudioProcessor; }

namespace context::ui
{
/** Prompt, sound type, bar length and the Generate action. */
class PromptPanel : public juce::Component
{
public:
    explicit PromptPanel (ContextAudioProcessor&);

    void refresh();
    void tick();
    void focusPrompt();
    void startGeneration();

    /** Generation errors are shown by the workspace (toast with Details). */
    std::function<void (const ProviderError&)> onError;
    std::function<void (const juce::String&)> onStatus;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void pushUiState();
    void updatePromptPreview();

    ContextAudioProcessor& processor;
    juce::TextEditor prompt;
    SegmentedControl typeSelector, lengthSelector;
    juce::TextButton generateButton { "Generate" }, cancelButton { "Cancel" };
    juce::TextButton useContext { "Analysis" }, sendAudio { "Send my track" }, expand { "Enhance" };
    SegmentedControl contextMode;          // Layer / Restyle / Continue (when sending the track)
    juce::StringArray modeIds;
    void askToAllowAudio();
    juce::String preview, status;
    double progress = -1.0;
    bool busy = false;
    juce::String lastFailedJobShown;
    juce::Rectangle<int> previewArea, statusArea, progressArea, lengthLabelArea, typeLabelArea, modeLabelArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PromptPanel)
};
} // namespace context::ui
