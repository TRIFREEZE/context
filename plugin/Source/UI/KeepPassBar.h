#pragma once

#include "Controls.h"
#include "Motion.h"

namespace context::ui
{
/** Floats over the sound lane when a new take lands: Keep it, or Pass on it.

      Keep   -> marked as kept (and a Favorite), so it stays in History.
      Pass   -> "What was off?" (Timing, Off key, Too busy, ...), then the take leaves the lane
                and the next takes of this prompt are steered away from it.

    It doesn't train the generator (Kie.ai/Suno can't be fine-tuned from here); it records your
    verdicts and steers prompts and negative tags with them. */
class KeepPassBar : public juce::Component
{
public:
    KeepPassBar();

    /** Ask about a take ("Take 3"). Animates in; a different take replaces the question. */
    void ask (const juce::String& generationId, const juce::String& takeId, const juce::String& takeLabel);
    /** Hides unless it is showing a result the user just produced. */
    void dismissQuestion();
    /** Straight to "What was off?" (Pass from the right-click menu). */
    void askWhy (const juce::String& generationId, const juce::String& takeId, const juce::String& takeLabel);
    const juce::String& generationId() const { return genId; }
    const juce::String& takeId() const { return take; }
    bool isAsking() const { return state == State::Ask || state == State::Reasons; }

    /** After the verdict: a short confirmation, then the bar leaves by itself. */
    void showKept();
    void showPassed (const juce::StringArray& steering, bool offerNewTake);

    std::function<void()> onKeep;
    std::function<void (const juce::StringArray& reasons)> onPass;
    std::function<void()> onNewTake;

    /** The width the bar wants in its current state (the panel centres it). */
    int idealWidth() const;
    std::function<void()> onLayoutChanged;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override { hovered = true; }
    void mouseExit (const juce::MouseEvent&) override { hovered = false; }

    // For tests / screenshots.
    juce::TextButton& keepButton() { return keep; }
    juce::TextButton& passButton() { return pass; }
    juce::TextButton* reasonButton (const juce::String& id);
    juce::TextButton& confirmPassButton() { return confirmPass; }

private:
    enum class State { Hidden, Ask, Reasons, Kept, Passed };
    void setState (State);
    void appear();
    void leave (float afterMs);
    void timerTick();

    struct Ticker : juce::Timer { std::function<void()> fn; void timerCallback() override { if (fn) fn(); } } ticker;

    State state = State::Hidden;
    juce::String genId, take, label, doneText, doneDetail;
    juce::TextButton keep { "Keep" }, pass { "Pass" }, confirmPass { "Pass" }, justPass { "Skip" }, newTake { "New take" };
    juce::OwnedArray<juce::TextButton> reasons;
    juce::StringArray reasonIds;
    motion::Value shown { 0.0f }, check { 0.0f };
    double leaveAtMs = 0.0;
    bool hovered = false;
};
} // namespace context::ui
