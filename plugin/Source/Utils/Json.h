#pragma once

#include <juce_core/juce_core.h>
#include <optional>

namespace context::json
{
/** Upper bounds applied to every JSON document coming from outside the process. */
inline constexpr int defaultMaxBytes = 2 * 1024 * 1024;
inline constexpr int defaultMaxDepth = 64;

struct ParseResult
{
    juce::var value;
    bool ok = false;
    juce::String error;
};

/** Parses untrusted JSON safely: rejects oversized documents and excessive nesting
    (which could exhaust the stack in a recursive parser) before parsing. Never throws. */
ParseResult parseUntrusted (const juce::String& text,
                            int maxBytes = defaultMaxBytes,
                            int maxDepth = defaultMaxDepth);

/** Returns the maximum bracket nesting depth outside of string literals. */
int nestingDepth (const juce::String& text);

/** Looks up a dotted path such as "data.0.audio_url". Array indices are plain numbers.
    Returns an undefined var if any step is missing or of the wrong type. */
juce::var getPath (const juce::var& root, const juce::String& dottedPath);

std::optional<juce::String> getString (const juce::var& root, const juce::String& path);
std::optional<double>       getNumber (const juce::var& root, const juce::String& path);
std::optional<bool>         getBool   (const juce::var& root, const juce::String& path);

/** True if the path syntax is acceptable for configuration files: segments of [A-Za-z0-9_-], dot separated. */
bool isValidFieldPath (const juce::String& path);

/** True only for JSON objects ({...}); juce::var::isObject() is also true for arrays. */
inline bool isObject (const juce::var& v) { return v.getDynamicObject() != nullptr; }

/** Serialises compactly (single line). */
juce::String toCompactString (const juce::var& v);
} // namespace context::json
