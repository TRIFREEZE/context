#include "EffectParameter.h"

namespace context::fx
{
ParameterSpec ParameterSpec::continuous (juce::String id, juce::String name, float min, float max, float def, Unit u, float centre, juce::String tip)
{
    ParameterSpec p;
    p.id = std::move (id);
    p.name = std::move (name);
    p.minValue = min;
    p.maxValue = max;
    p.defaultValue = def;
    p.unit = u;
    p.centreValue = centre;
    p.tooltip = std::move (tip);
    return p;
}

ParameterSpec ParameterSpec::choice (juce::String id, juce::String name, juce::StringArray choices, int def, juce::String tip)
{
    ParameterSpec p;
    p.id = std::move (id);
    p.name = std::move (name);
    p.choices = std::move (choices);
    p.minValue = 0.0f;
    p.maxValue = (float) juce::jmax (0, p.choices.size() - 1);
    p.defaultValue = (float) def;
    p.step = 1.0f;
    p.tooltip = std::move (tip);
    return p;
}

ParameterSpec ParameterSpec::onOff (juce::String id, juce::String name, bool def, juce::String tip)
{
    ParameterSpec p;
    p.id = std::move (id);
    p.name = std::move (name);
    p.toggle = true;
    p.minValue = 0.0f;
    p.maxValue = 1.0f;
    p.defaultValue = def ? 1.0f : 0.0f;
    p.step = 1.0f;
    p.tooltip = std::move (tip);
    return p;
}

EffectParameter::EffectParameter (ParameterSpec spec) : s (std::move (spec))
{
    jassert (s.maxValue > s.minValue || s.toggle || s.choices.size() == 1);
    range = juce::NormalisableRange<float> (s.minValue, juce::jmax (s.maxValue, s.minValue + 1.0e-6f), s.step);
    if (s.centreValue > s.minValue && s.centreValue < s.maxValue)
        range.setSkewForCentre (s.centreValue);
    value.store (clamp (s.defaultValue));
}

float EffectParameter::clamp (float v) const noexcept
{
    if (! std::isfinite (v))
        v = s.defaultValue;
    v = juce::jlimit (s.minValue, juce::jmax (s.minValue, s.maxValue), v);
    if (s.step > 0.0f)
        v = s.minValue + std::round ((v - s.minValue) / s.step) * s.step;
    return v;
}

void EffectParameter::set (float v) noexcept
{
    value.store (clamp (v), std::memory_order_relaxed);
}

float EffectParameter::toNormalised (float v) const
{
    return range.convertTo0to1 (clamp (v));
}

float EffectParameter::fromNormalised (float n) const
{
    return clamp (range.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, n)));
}

juce::String EffectParameter::toText (float v) const
{
    if (isChoice())
        return s.choices[juce::jlimit (0, s.choices.size() - 1, juce::roundToInt (v))];
    if (isToggle())
        return v >= 0.5f ? "On" : "Off";

    auto num = [] (float x, int decimals) { return juce::String (x, decimals); };
    switch (s.unit)
    {
        case Unit::Decibels:
            if (v <= -79.9f && s.minValue <= -80.0f) return juce::String (juce::CharPointer_UTF8 ("-\xe2\x88\x9e dB"));
            return (v > 0.0f ? "+" : "") + num (v, std::abs (v) < 10.0f ? 1 : 0) + " dB";
        case Unit::Hertz:        return v >= 1000.0f ? num (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz" : num (v, v < 100.0f ? 1 : 0) + " Hz";
        case Unit::Milliseconds: return v >= 1000.0f ? num (v / 1000.0f, 2) + " s" : num (v, v < 10.0f ? 2 : (v < 100.0f ? 1 : 0)) + " ms";
        case Unit::Seconds:      return num (v, v < 10.0f ? 2 : 1) + " s";
        case Unit::Percent:      return num (v, 0) + " %";
        case Unit::Ratio:        return v >= 20.0f ? juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x9e:1")) : num (v, v < 10.0f ? 1 : 0) + ":1";
        case Unit::Semitones:    return (v > 0.0f ? "+" : "") + num (v, s.step >= 1.0f ? 0 : 1) + " st";
        case Unit::Bits:         return num (v, s.step >= 1.0f ? 0 : 1) + " bit";
        case Unit::Multiplier:   return num (v, 2) + "x";
        case Unit::None:         break;
    }
    return num (v, 2);
}

std::optional<float> EffectParameter::fromText (const juce::String& text) const
{
    auto t = text.trim();
    if (t.isEmpty())
        return std::nullopt;
    if (isChoice())
    {
        for (int i = 0; i < s.choices.size(); ++i)
            if (s.choices[i].equalsIgnoreCase (t))
                return (float) i;
        return std::nullopt;
    }
    if (isToggle())
    {
        if (t.equalsIgnoreCase ("on") || t == "1") return 1.0f;
        if (t.equalsIgnoreCase ("off") || t == "0") return 0.0f;
        return std::nullopt;
    }
    const auto lower = t.toLowerCase();
    const auto digits = t.retainCharacters ("0123456789.-+");
    if (digits.isEmpty() || ! digits.containsAnyOf ("0123456789"))
        return std::nullopt;
    float v = digits.getFloatValue();
    if (s.unit == Unit::Hertz && lower.contains ("k"))
        v *= 1000.0f;
    if (s.unit == Unit::Milliseconds && lower.endsWith ("s") && ! lower.endsWith ("ms"))
        v *= 1000.0f;
    return clamp (v);
}
} // namespace context::fx
