#pragma once

#include "GenerationModels.h"

namespace context
{
/** Turns "dark distorted female vocal chop" + track analysis into a richer,
    context-aware prompt, using simple, explainable heuristics.

    Rules of thumb:
      - user intent always wins: an explicit BPM/key in the prompt is never
        overridden, and "huge sub bass" is never told to avoid low end
      - the generation type decides which hints make sense
        (no key for a hi-hat, no "loop cleanly" for a one-shot)
      - each hint is recorded so the UI can explain what was added */
class PromptBuilder
{
public:
    struct Inputs
    {
        juce::String userPrompt;
        GenerationType type = GenerationType::OneShot;
        std::optional<TrackAnalysis> analysis;    // set only when "Use Track Context" is on
        std::optional<double> hostBpm;            // DAW tempo, used when "Use Track Context" is on
        bool expansionEnabled = true;             // "Context Prompt Expansion"
    };

    struct Output
    {
        juce::String expandedPrompt;              // what is sent as the prompt
        juce::StringArray hints;                  // short tags describing what was added
        std::optional<double> bpm;                // structured fields for providers
        juce::String key, mode;                   // "F#", "minor"
        std::optional<double> keyConfidence;
    };

    static Output build (const Inputs&);

    // Exposed for tests
    struct Intent
    {
        std::optional<double> explicitBpm;
        bool explicitKey = false;
        bool wantsLowEnd = false, wantsBright = false, wantsDark = false;
        bool wantsWide = false, wantsMono = false, wantsShort = false, wantsSustained = false;
        bool drumSubject = false, tonalDrum = false;
    };
    static Intent detectIntent (const juce::String& prompt);

    static constexpr int maxLength = 900;
};
} // namespace context
