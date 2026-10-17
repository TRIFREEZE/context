#include "FilenameSanitizer.h"

namespace context::filenames
{
namespace
{
    bool isReservedWindowsName (const juce::String& nameWithoutExt)
    {
        const auto upper = nameWithoutExt.toUpperCase();
        static const juce::StringArray reserved { "CON", "PRN", "AUX", "NUL",
            "COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9",
            "LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9" };
        return reserved.contains (upper);
    }
}

juce::String sanitizeComponent (const juce::String& input, int maxLength)
{
    juce::String out;
    out.preallocateBytes ((size_t) input.getNumBytesAsUTF8() + 1);
    bool lastWasUnderscore = false;

    for (auto p = input.getCharPointer(); ! p.isEmpty(); ++p)
    {
        const juce::juce_wchar c = *p;
        const bool invalid = c < 32 || c == 127
                          || c == '<' || c == '>' || c == ':' || c == '"' || c == '/'
                          || c == '\\' || c == '|' || c == '?' || c == '*'
                          || juce::CharacterFunctions::isWhitespace (c);
        if (invalid)
        {
            if (! lastWasUnderscore && out.isNotEmpty())
                out << '_';
            lastWasUnderscore = true;
        }
        else
        {
            out << juce::String::charToString (c);
            lastWasUnderscore = (c == '_');
        }
    }

    // Avoid "." / ".." and Windows' silent stripping of trailing dots and spaces.
    while (out.contains (".."))
        out = out.replace ("..", ".");
    out = out.trimCharactersAtEnd (". _").trimCharactersAtStart (". _");

    if (out.length() > maxLength)
        out = out.substring (0, maxLength).trimCharactersAtEnd (". _");

    if (out.isEmpty())
        out = "untitled";

    const auto stem = out.upToFirstOccurrenceOf (".", false, false);
    if (isReservedWindowsName (stem))
        out = "_" + out;

    return out;
}

juce::String keyForFilename (const juce::String& keyName)
{
    auto k = keyName.trim();
    k = k.replace ("#", "Sharp").replace (juce::CharPointer_UTF8 ("\xe2\x99\xaf"), "Sharp")
         .replace (juce::CharPointer_UTF8 ("\xe2\x99\xad"), "Flat");
    if (k.length() == 2 && k[1] == 'b')
        k = k.substring (0, 1) + "Flat";
    return sanitizeComponent (k, 16);
}

juce::String generatedBaseName (const juce::String& productName,
                                std::optional<double> bpm,
                                const juce::String& keyName,
                                const juce::String& modeName,
                                const juce::String& typeName,
                                juce::Time when)
{
    juce::StringArray parts;
    parts.add (sanitizeComponent (productName, 32));

    if (bpm.has_value() && *bpm >= 20.0 && *bpm <= 999.0)
        parts.add (juce::String (juce::roundToInt (*bpm)) + "BPM");

    if (keyName.isNotEmpty())
        parts.add (keyForFilename (keyName) + sanitizeComponent (modeName.isNotEmpty() ? modeName : juce::String(), 8).replace ("untitled", ""));

    if (typeName.isNotEmpty())
        parts.add (sanitizeComponent (typeName.removeCharacters (" -"), 24));

    parts.add (when.formatted ("%Y-%m-%d"));
    return sanitizeComponent (parts.joinIntoString ("_"), 120);
}

juce::File uniqueFile (const juce::File& dir, const juce::String& base, const juce::String& extensionWithDot)
{
    const auto safeBase = sanitizeComponent (base, 120);
    for (int i = 1; i < 100000; ++i)
    {
        auto f = dir.getChildFile (safeBase + "_" + juce::String (i).paddedLeft ('0', 3) + extensionWithDot);
        if (! f.exists())
            return f;
    }
    return dir.getNonexistentChildFile (safeBase, extensionWithDot, false);
}

bool isInsideDirectory (const juce::File& child, const juce::File& parent)
{
    if (child == juce::File() || parent == juce::File())
        return false;

    const auto c = child.getFullPathName();
    const auto p = parent.getFullPathName();
    // juce::File already normalises "..", "." and separators for absolute paths.
    return child.isAChildOf (parent) && c.startsWithIgnoreCase (p) && c != p;
}
} // namespace context::filenames
