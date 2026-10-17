#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <optional>

namespace context::fx
{
enum class Unit { None, Decibels, Hertz, Milliseconds, Percent, Ratio, Semitones, Bits, Multiplier, Seconds };

/** Static description of one effect parameter. */
struct ParameterSpec
{
    juce::String id;                 // stable, used in saved state ("threshold")
    juce::String name;               // shown under the knob ("Threshold")
    float minValue = 0.0f, maxValue = 1.0f, defaultValue = 0.0f;
    float centreValue = 0.0f;        // value at the middle of the knob (log-style skew); 0 = linear
    float step = 0.0f;               // 0 = continuous
    Unit unit = Unit::None;
    juce::StringArray choices;       // non-empty: a choice parameter (value = index)
    bool toggle = false;             // on/off parameter (0/1)
    juce::String tooltip;

    static ParameterSpec continuous (juce::String id, juce::String name, float min, float max, float def, Unit u,
                                     float centre = 0.0f, juce::String tip = {});
    static ParameterSpec choice (juce::String id, juce::String name, juce::StringArray choices, int def, juce::String tip = {});
    static ParameterSpec onOff (juce::String id, juce::String name, bool def, juce::String tip = {});
};

/** A live parameter value. Written by the UI (message thread), read lock-free by DSP
    (audio thread) and the offline renderer. Values are always kept in range. */
class EffectParameter
{
public:
    explicit EffectParameter (ParameterSpec);

    const ParameterSpec& spec() const noexcept { return s; }
    const juce::String& id() const noexcept    { return s.id; }

    float get() const noexcept                 { return value.load (std::memory_order_relaxed); }
    int getIndex() const noexcept              { return juce::roundToInt (get()); }
    bool getBool() const noexcept              { return get() >= 0.5f; }
    void set (float v) noexcept;
    void reset() noexcept                      { set (s.defaultValue); }
    bool isDefault() const noexcept            { return std::abs (get() - s.defaultValue) < 1.0e-6f; }

    bool isChoice() const noexcept             { return s.choices.size() > 0; }
    bool isToggle() const noexcept             { return s.toggle; }
    bool isContinuous() const noexcept         { return ! isChoice() && ! isToggle(); }

    float toNormalised (float v) const;
    float fromNormalised (float n) const;
    juce::NormalisableRange<float> getRange() const { return range; }

    juce::String toText (float v) const;
    juce::String currentText() const           { return toText (get()); }
    std::optional<float> fromText (const juce::String&) const;

    float clamp (float v) const noexcept;

private:
    ParameterSpec s;
    juce::NormalisableRange<float> range;
    std::atomic<float> value { 0.0f };
};
} // namespace context::fx
