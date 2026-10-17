#include "Redaction.h"
#include <regex>

namespace context::redaction
{
namespace
{
    std::string replaceAll (const std::string& in, const std::regex& re, const std::string& fmt)
    {
        return std::regex_replace (in, re, fmt);
    }

    const std::regex& licenseKeyRe()
    {
        static const std::regex re (R"(\b(CTX)-([A-Z0-9]{4})-([A-Z0-9]{4})-([A-Z0-9]{4})-([A-Z0-9]{4})\b)", std::regex::icase);
        return re;
    }
}

juce::String maskLicenseKey (const juce::String& key)
{
    const auto k = key.trim().toUpperCase();
    if (k.length() <= 4)
        return "****";

    const bool hasPrefix = k.startsWith ("CTX-");
    juce::String out;
    for (int i = 0; i < k.length(); ++i)
    {
        const auto c = k[i];
        if (i >= k.length() - 4 || c == '-' || (hasPrefix && i < 4))
            out << juce::String::charToString (c);
        else
            out << '*';
    }
    return out;
}

juce::String safeUrlForDisplay (const juce::String& url)
{
    auto u = url.trim();
    const int schemeEnd = u.indexOf ("://");
    if (schemeEnd > 0)
    {
        const auto afterScheme = u.substring (schemeEnd + 3);
        const int slash = afterScheme.indexOfChar ('/');
        auto authority = slash >= 0 ? afterScheme.substring (0, slash) : afterScheme;
        const auto rest = slash >= 0 ? afterScheme.substring (slash) : juce::String();
        if (authority.containsChar ('@'))
            authority = authority.fromLastOccurrenceOf ("@", false, false);
        u = u.substring (0, schemeEnd + 3) + authority + rest;
    }
    return u.upToFirstOccurrenceOf ("?", false, false).upToFirstOccurrenceOf ("#", false, false);
}

juce::String redact (const juce::String& text)
{
    if (text.isEmpty())
        return text;

    // Bound the work (and std::regex recursion depth) for pathological inputs.
    constexpr int maxChars = 8192;
    const auto bounded = text.length() > maxChars ? text.substring (0, maxChars) + " ...[truncated]" : text;

    try
    {
        std::string s = bounded.toStdString();

        // License keys: keep the prefix and the last block only.
        s = replaceAll (s, licenseKeyRe(), "$1-****-****-****-$5");

        // Authorization / Cookie style headers (to end of line).
        static const std::regex headerRe (R"(((?:authorization|proxy-authorization|cookie|set-cookie|x-api-key)\s*:\s*)[^\r\n]*)", std::regex::icase);
        s = replaceAll (s, headerRe, "$1[REDACTED]");

        // Bearer tokens anywhere.
        static const std::regex bearerRe (R"((bearer\s+)[A-Za-z0-9\-\._~\+\/]+=*)", std::regex::icase);
        s = replaceAll (s, bearerRe, "$1[REDACTED]");

        // JSON "key": "value" for sensitive keys.
        static const std::regex jsonRe (R"rx(("(?:[a-z_\-]*(?:token|secret|password|passwd|cookie|session|api[_\-]?key|auth|credential|license_key|licenseKey)[a-z_\-]*)"\s*:\s*)"[^"]*")rx", std::regex::icase);
        s = replaceAll (s, jsonRe, "$1\"[REDACTED]\"");

        // key=value pairs (query strings, form bodies, env-style).
        static const std::regex kvRe (R"(((?:^|[?&;\s,])(?:[a-z_\-]*(?:token|secret|password|passwd|cookie|session|api[_\-]?key|auth|credential)[a-z_\-]*)=)[^&;\s,]*)", std::regex::icase);
        s = replaceAll (s, kvRe, "$1[REDACTED]");

        // JWT-like tokens.
        static const std::regex jwtRe (R"(\beyJ[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{8,}\b)");
        s = replaceAll (s, jwtRe, "[REDACTED-JWT]");

        // Credentials in URLs.
        static const std::regex urlCredRe (R"(([a-z][a-z0-9+\-.]*://)[^/\s:@]+:[^/\s@]+@)", std::regex::icase);
        s = replaceAll (s, urlCredRe, "$1[REDACTED]@");

        return juce::String (juce::CharPointer_UTF8 (s.c_str()));
    }
    catch (...)
    {
        return "[log line withheld: redaction failed]";
    }
}
} // namespace context::redaction
