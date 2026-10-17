#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>

/** Motion for the whole interface.

    Two tools:
      * follow()   for look-and-feel drawing (hover fades, toggle fills, knob arcs): the
                   animated value is derived from time at paint time and stored in the
                   component's properties, so every JUCE control animates without subclassing.
      * Value      an animated float owned by a component (segmented-control pill, chip
                   positions, toasts, sheets). Values register with the Clock while they move.

    One Clock drives everything from the display's vertical blank (VBlankAttachment on the
    editor), with a 60 Hz timer as a fallback when there is no display link. When motion is
    disabled ("Reduce motion" in Settings, and in tests) every animation jumps to its end. */
namespace context::ui::motion
{
bool enabled() noexcept;
/** The user's preference (Settings > Reduce motion). */
void setEnabled (bool) noexcept;
/** Process-wide switch, for tests and screenshots: when false, nothing ever animates. */
void setAllowed (bool) noexcept;
double nowMs() noexcept;

enum class Ease { Linear, OutCubic, InOutCubic, OutQuint, OutBack };
float apply (Ease, float t) noexcept;

namespace duration
{
    inline constexpr float hover = 110.0f, press = 60.0f, toggle = 160.0f, slide = 220.0f, panel = 260.0f, fade = 180.0f;
}

/** Animated property for paint code: returns the current value moving toward `target`. */
float follow (juce::Component&, const juce::Identifier& key, float target, float ms = duration::hover, Ease = Ease::OutCubic);
/** Common case: 0..1 hover amount (and press) for a control. */
float hoverAmount (juce::Component&, bool over);
float pressAmount (juce::Component&, bool down);

/** Something the clock ticks; return true from tick() while still moving. */
class Animated
{
public:
    virtual ~Animated();
    void startAnimating();
    void stopAnimating();
    bool isAnimating() const noexcept { return running; }

protected:
    virtual bool tick (double nowMs) = 0;

private:
    friend class Clock;
    bool running = false;
};

/** An animated float. Call set() to move, get() to read; `onChange` runs every frame. */
class Value : public Animated
{
public:
    explicit Value (float initial = 0.0f) : current (initial), from (initial), to (initial) {}
    void set (float target, float ms = duration::slide, Ease = Ease::OutCubic);
    void snap (float v);
    float get() const noexcept { return current; }
    float target() const noexcept { return to; }
    std::function<void()> onChange;

private:
    bool tick (double nowMs) override;
    float current, from, to;
    double start = 0.0;
    float length = 0.0f;
    Ease ease = Ease::OutCubic;
};

/** A rectangle animated as four Values (layout transitions). */
class Rect
{
public:
    void set (juce::Rectangle<float> target, float ms = duration::slide, Ease = Ease::OutCubic);
    void snap (juce::Rectangle<float>);
    juce::Rectangle<float> get() const;
    bool isAnimating() const { return x.isAnimating() || y.isAnimating() || w.isAnimating() || h.isAnimating(); }
    void onChange (std::function<void()> fn) { x.onChange = fn; y.onChange = fn; w.onChange = fn; h.onChange = fn; }
    bool initialised = false;

private:
    Value x, y, w, h;
};

/** The frame clock. The editor attaches it to its vblank; nothing else needs to know. */
class Clock : private juce::Timer, public juce::DeletedAtShutdown
{
public:
    static Clock& instance();
    /** Null after JUCE shut down (Animated destructors must not resurrect the clock). */
    static Clock* existing();
    ~Clock() override;
    void add (Animated*);
    void remove (Animated*);
    void repaintUntil (juce::Component&, double endMs);
    /** Ticks from the component's display refresh while it is on screen. */
    std::unique_ptr<juce::VBlankAttachment> attach (juce::Component& top);
    void tick();

private:
    Clock() = default;
    void timerCallback() override;
    static Clock* instancePtr;
    void ensureRunning();
    std::vector<Animated*> items;
    std::vector<std::pair<juce::Component::SafePointer<juce::Component>, double>> repaints;
    double lastVBlank = -1.0e9;
    bool ticking = false;
};
} // namespace context::ui::motion
