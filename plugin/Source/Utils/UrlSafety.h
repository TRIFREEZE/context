#pragma once

#include <juce_core/juce_core.h>

namespace context::urls
{
enum class HostKind { Loopback, PrivateNetwork, Public, Invalid };

struct ParsedUrl
{
    bool ok = false;
    juce::String error;          // human readable reason when !ok
    juce::String scheme;         // "http" / "https"
    juce::String host;           // without brackets
    int port = 0;                // explicit or default
    juce::String normalized;     // scheme://host[:port] (no trailing slash, no path)
    HostKind kind = HostKind::Invalid;
};

/** Parses a bridge / server base URL. Only http/https, no credentials, no query,
    no fragment. A path other than "/" is rejected (paths come from the bridge profile). */
ParsedUrl parseBaseUrl (const juce::String& text);

HostKind classifyHost (const juce::String& host);
bool isLoopbackHost (const juce::String& host);

/** Joins a validated base URL with a profile path ("/x/{id}"), percent-encoding the id. */
juce::String joinPath (const juce::String& normalizedBase, const juce::String& path, const juce::String& jobId = {});

/** Policy for audio download URLs returned by a bridge:
    https anywhere; http only to loopback (or to the bridge's own host).
    Relative URLs ("/files/x.wav") are resolved against the bridge base. */
struct DownloadUrlCheck { bool ok = false; juce::String url; juce::String error; };
DownloadUrlCheck checkDownloadUrl (const juce::String& candidate, const ParsedUrl& bridgeBase);

juce::String describe (HostKind);
} // namespace context::urls
