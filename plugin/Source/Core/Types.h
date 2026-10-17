#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <optional>

namespace context
{
enum class GenerationType { OneShot, Loop, Vocal, Instrument, Percussion, Texture, FX, Melody };

inline constexpr std::array<GenerationType, 8> allGenerationTypes {
    GenerationType::OneShot, GenerationType::Loop, GenerationType::Vocal, GenerationType::Instrument,
    GenerationType::Percussion, GenerationType::Texture, GenerationType::FX, GenerationType::Melody };

/** Stable identifier used in JSON, settings and history ("one_shot", ...). */
inline juce::String toId (GenerationType t)
{
    switch (t)
    {
        case GenerationType::OneShot:    return "one_shot";
        case GenerationType::Loop:       return "loop";
        case GenerationType::Vocal:      return "vocal";
        case GenerationType::Instrument: return "instrument";
        case GenerationType::Percussion: return "percussion";
        case GenerationType::Texture:    return "texture";
        case GenerationType::FX:         return "fx";
        case GenerationType::Melody:     return "melody";
    }
    return "one_shot";
}

/** Human readable label ("One Shot"). */
inline juce::String toLabel (GenerationType t)
{
    switch (t)
    {
        case GenerationType::OneShot:    return "One Shot";
        case GenerationType::Loop:       return "Loop";
        case GenerationType::Vocal:      return "Vocal";
        case GenerationType::Instrument: return "Instrument";
        case GenerationType::Percussion: return "Percussion";
        case GenerationType::Texture:    return "Texture";
        case GenerationType::FX:         return "FX";
        case GenerationType::Melody:     return "Melody";
    }
    return "One Shot";
}

inline std::optional<GenerationType> generationTypeFromId (const juce::String& id)
{
    for (auto t : allGenerationTypes)
        if (toId (t) == id.trim().toLowerCase())
            return t;
    return std::nullopt;
}
} // namespace context
