#include "Json.h"

namespace context::json
{
int nestingDepth (const juce::String& text)
{
    int depth = 0, maxDepth = 0;
    bool inString = false, escaped = false;

    for (auto p = text.getCharPointer(); ! p.isEmpty(); ++p)
    {
        const auto c = *p;
        if (inString)
        {
            if (escaped)          escaped = false;
            else if (c == '\\')   escaped = true;
            else if (c == '"')    inString = false;
            continue;
        }

        if (c == '"')                  inString = true;
        else if (c == '{' || c == '[') maxDepth = juce::jmax (maxDepth, ++depth);
        else if (c == '}' || c == ']') depth = juce::jmax (0, depth - 1);
    }
    return maxDepth;
}

ParseResult parseUntrusted (const juce::String& text, int maxBytes, int maxDepth)
{
    ParseResult r;

    if ((int) text.getNumBytesAsUTF8() > maxBytes)
    {
        r.error = "JSON document too large (" + juce::String ((int) text.getNumBytesAsUTF8()) + " bytes)";
        return r;
    }

    if (text.trim().isEmpty())
    {
        r.error = "Empty response";
        return r;
    }

    if (nestingDepth (text) > maxDepth)
    {
        r.error = "JSON nesting too deep";
        return r;
    }

    juce::var parsed;
    const auto result = juce::JSON::parse (text, parsed);
    if (result.failed())
    {
        r.error = "Malformed JSON: " + result.getErrorMessage().substring (0, 200);
        return r;
    }

    r.value = parsed;
    r.ok = true;
    return r;
}

juce::var getPath (const juce::var& root, const juce::String& dottedPath)
{
    if (dottedPath.isEmpty())
        return root;

    juce::var current = root;
    for (const auto& seg : juce::StringArray::fromTokens (dottedPath, ".", ""))
    {
        if (seg.isEmpty())
            return {};

        if (current.isArray() && seg.containsOnly ("0123456789"))
        {
            const int idx = seg.getIntValue();
            auto* arr = current.getArray();
            if (arr == nullptr || idx < 0 || idx >= arr->size())
                return {};
            current = arr->getReference (idx);
        }
        else if (auto* obj = current.getDynamicObject())
        {
            const juce::Identifier id (seg);
            if (! obj->hasProperty (id))
                return {};
            current = obj->getProperty (id);
        }
        else
        {
            return {};
        }
    }
    return current;
}

std::optional<juce::String> getString (const juce::var& root, const juce::String& path)
{
    const auto v = getPath (root, path);
    if (v.isString())
        return v.toString();
    if (v.isInt() || v.isInt64() || v.isDouble())
        return v.toString();
    return std::nullopt;
}

std::optional<double> getNumber (const juce::var& root, const juce::String& path)
{
    const auto v = getPath (root, path);
    if (v.isInt() || v.isInt64() || v.isDouble())
    {
        const double d = (double) v;
        if (std::isfinite (d))
            return d;
        return std::nullopt;
    }
    if (v.isString())
    {
        const auto s = v.toString().trim();
        if (s.isNotEmpty() && s.containsOnly ("0123456789.-+eE"))
        {
            const double d = s.getDoubleValue();
            if (std::isfinite (d))
                return d;
        }
    }
    return std::nullopt;
}

std::optional<bool> getBool (const juce::var& root, const juce::String& path)
{
    const auto v = getPath (root, path);
    if (v.isBool())
        return (bool) v;
    return std::nullopt;
}

bool isValidFieldPath (const juce::String& path)
{
    if (path.isEmpty() || path.length() > 128)
        return false;
    for (const auto& seg : juce::StringArray::fromTokens (path, ".", ""))
        if (seg.isEmpty() || ! seg.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-"))
            return false;
    return ! path.startsWithChar ('.') && ! path.endsWithChar ('.');
}

juce::String toCompactString (const juce::var& v)
{
    return juce::JSON::toString (v, juce::JSON::FormatOptions{}.withSpacing (juce::JSON::Spacing::none));
}
} // namespace context::json
