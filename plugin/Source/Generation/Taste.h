#pragma once

#include "GenerationModels.h"

namespace context::taste
{
/** What you can say was off about a take you passed on. Each one steers the next takes of the
    same prompt: words added to the prompt (when Enhance is on) and things to avoid (sent to the
    bridge as negative tags, which Kie.ai's models accept). This is steering, not training: the
    generator itself doesn't change. */
struct Reason
{
    const char* id;       // stored in history
    const char* label;    // chip text
    const char* steer;    // added to the prompt
    const char* avoid;    // negative tags
};
const std::vector<Reason>& reasons();
const Reason* findReason (const juce::String& id);

struct Steer
{
    juce::StringArray hints;          // prompt phrases, most-mentioned first
    juce::StringArray avoid;          // negative tags
    juce::StringArray labels;         // for the UI ("Timing", "Too busy")
    int passes = 0, keeps = 0;        // verdicts on this prompt so far
    bool isEmpty() const { return hints.isEmpty() && avoid.isEmpty(); }
};

/** The steer for a prompt + type, from your verdicts in history (newest first; the last 40
    verdicts on the same prompt count). Keeping a take doesn't undo the steer that made it. */
Steer forPrompt (const std::vector<GenerationResult>& history, const juce::String& prompt, GenerationType);

/** Your verdicts as a JSON document (prompt, type, tempo, key, verdict, reasons, file), for
    keeping or for training a model of your own later. Contains no audio and no credentials. */
juce::String exportRatings (const std::vector<GenerationResult>& history);

inline int ratingOf (const GenerationResult& r) { return (int) r.metadata.getWithDefault ("rating", 0); }
} // namespace context::taste
