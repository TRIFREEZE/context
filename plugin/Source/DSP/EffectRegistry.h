#pragma once

#include "EffectProcessor.h"
#include <functional>

namespace context::fx
{
/** One effect type the rack can create. */
struct EffectType
{
    juce::String id;            // stable id saved in projects ("compressor")
    juce::String name;          // "Compressor"
    juce::String shortName;     // rack label ("COMP")
    juce::String category;      // "Dynamics", "EQ & Filter", "Saturation", "Space", "Modulation", "Lo-Fi", "Utility"
    juce::String description;   // one-line tooltip
    std::function<std::unique_ptr<EffectProcessor>()> create;
};

/** Factory for effect types. The rack and the state loader only ever create effects
    through a registry, so adding an effect never touches UI or chain code:
        registry.registerEffect ({ "granular", "Granular", "GRAIN", "Space", "...",
                                   [] { return std::make_unique<GranularEffect>(); } });

    builtIn() is built once on first use and never changes afterwards (read-only, so it
    is safe to share between plugin instances and threads). */
class EffectRegistry
{
public:
    void registerEffect (EffectType);

    const EffectType* find (const juce::String& id) const noexcept;
    const std::vector<EffectType>& types() const noexcept { return all; }
    std::unique_ptr<EffectProcessor> create (const juce::String& id) const;
    juce::StringArray categories() const;

    static const EffectRegistry& builtIn();

private:
    std::vector<EffectType> all;
};

/** Registers every effect that ships with Context (BuiltInEffects.cpp). */
void registerBuiltInEffects (EffectRegistry&);
} // namespace context::fx
