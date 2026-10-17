#pragma once

#include <juce_core/juce_core.h>

namespace context::redaction
{
/** Masks anything that looks secret before it can reach a log, the clipboard or the UI:
    Context license keys (keeps last 4 chars), bearer tokens, Authorization / Cookie
    headers, cookie/session/token/password/api_key style key=value or "key": "value"
    pairs, JWT-like strings, and credentials embedded in URLs (user:pass@host). */
juce::String redact (const juce::String& text);

/** "CTX-ABCD-EFGH-IJKL-MNOP" -> "CTX-****-****-****-MNOP". */
juce::String maskLicenseKey (const juce::String& key);

/** Removes user:password@ from a URL and drops its query string. */
juce::String safeUrlForDisplay (const juce::String& url);
} // namespace context::redaction
