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
/** A pill-shaped button for floating cards: sits concentric in a pill, icon + text centred as one. */
class PillButton : public juce::Button
{
public:
    enum class Kind { Primary, Secondary, Ghost, Toggle };
    PillButton (const juce::String& text, Kind k, std::optional<IconButton::Icon> icon = std::nullopt);
    int idealWidth() const;
    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    Kind kind;
    std::optional<IconButton::Icon> icon;
};

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
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override { hovered = true; }
    void mouseExit (const juce::MouseEvent&) override { hovered = false; }

    // For tests / screenshots.
    juce::Button& keepButton() { return keep; }
    juce::Button& passButton() { return pass; }
    juce::Button* reasonButton (const juce::String& id);
    juce::Button& confirmPassButton() { return confirmPass; }

private:
    enum class State { Hidden, Ask, Reasons, Kept, Passed };
    void setState (State);
    void appear();
    void leave (float afterMs);
    void timerTick();

    struct Ticker : juce::Timer { std::function<void()> fn; void timerCallback() override { if (fn) fn(); } } ticker;

    State state = State::Hidden;
    juce::String genId, take, label, doneText, doneDetail;
    PillButton keep { "Keep", PillButton::Kind::Primary, IconButton::Icon::Check },
               pass { "Pass", PillButton::Kind::Secondary, IconButton::Icon::Close },
               confirmPass { "Pass", PillButton::Kind::Secondary, IconButton::Icon::Close },
               justPass { "Skip", PillButton::Kind::Ghost },
               newTake { "New take", PillButton::Kind::Primary, IconButton::Icon::Sparkle };
    juce::OwnedArray<PillButton> reasons;
    juce::StringArray reasonIds;
    motion::Value shown { 0.0f }, check { 0.0f };
    double leaveAtMs = 0.0;
    bool hovered = false;
};
} // namespace context::ui
