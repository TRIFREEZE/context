#pragma once

#include <juce_core/juce_core.h>
#include <optional>

namespace context::filenames
{
/** Makes one path component safe on Windows, macOS and Linux:
    removes < > : " / \ | ? * and control characters, collapses whitespace to '_',
    strips trailing dots/spaces, avoids reserved device names (CON, NUL, COM1 ...),
    and limits length. Never returns an empty string. */
juce::String sanitizeComponent (const juce::String& input, int maxLength = 80);

/** "F#" -> "FSharp", "Bb" -> "BFlat", "C" -> "C". */
juce::String keyForFilename (const juce::String& keyName);

/** Context_142BPM_FSharpMinor_Vocal_2026-10-16  (index and extension added by uniqueFile). */
juce::String generatedBaseName (const juce::String& productName,
                                std::optional<double> bpm,
                                const juce::String& keyName,      // empty if unknown
                                const juce::String& modeName,     // "Major"/"Minor"/empty
                                const juce::String& typeName,
                                juce::Time when);

/** Returns dir/<base>_NNN<ext> that does not exist yet (NNN from 001). */
juce::File uniqueFile (const juce::File& dir, const juce::String& base, const juce::String& extensionWithDot);

/** True if `child` is inside `parent` after resolving to absolute, normalised paths. */
bool isInsideDirectory (const juce::File& child, const juce::File& parent);
} // namespace context::filenames
