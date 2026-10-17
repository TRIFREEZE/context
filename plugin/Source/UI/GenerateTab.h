#pragma once

#include "WaveformView.h"
#include "Generation/GenerationModels.h"

namespace context
{
class ContextAudioProcessor;
}

namespace context::ui
{
/** The main screen: prompt -> analyze -> generate -> listen -> drag. */
class GenerateTab : public juce::Component
{
public:
    explicit GenerateTab (ContextAudioProcessor&);
    ~GenerateTab() override;

    void refresh();             // state changed (message thread)
    void tick();                // ~20 Hz: progress + playhead
    void focusPrompt();

    std::function<void()> onOpenSettings;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void pushUiState();
    void startGeneration();
    void showError (const ProviderError&);
    void clearError();
    void showDetails();
    void saveAs();
    std::optional<GenerationResult> currentResult() const;
    void updateTypeButtons();
    void updatePromptPreview();

    ContextAudioProcessor& processor;

    juce::TextEditor prompt;
    std::array<juce::TextButton, 8> typeButtons;
    juce::ToggleButton useContext { "Use Track Analysis" }, sendAudio { "Send Audio Context" }, expand { "Enhance Prompt" };
    juce::TextButton generateButton { "GENERATE" }, cancelButton { "Cancel" };
    juce::TextEditor enhancedPrompt;
    juce::Label statusLabel;

    juce::TextButton analyzeButton { "Analyze Track" };
    juce::Label analysisMessage;

    WaveformView waveform;
    juce::TextButton playButton { "Play" }, stopButton { "Stop" }, favoriteButton, regenerateButton { "Regenerate" },
                     variationButton { "Variation" }, revealButton { "Show in Folder" }, copyPathButton { "Copy Path" },
                     saveAsButton { "Save As..." };
    juce::Label resultTitle, resultMeta;

    juce::Label errorLabel;
    juce::TextButton detailsButton { "Details" }, dismissButton { "Dismiss" };
    ProviderError lastError;
    juce::String lastFailedJobShown;
    bool errorVisible = false;

    std::unique_ptr<juce::FileChooser> chooser;

    // cached for painting
    juce::Rectangle<int> leftCard, analysisCard, resultCard, statusArea, analysisRows;
    double progress = -1.0;
    bool busy = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GenerateTab)
};
} // namespace context::ui
