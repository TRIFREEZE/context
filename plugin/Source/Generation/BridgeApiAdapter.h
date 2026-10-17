#pragma once

#include "BridgeConfiguration.h"
#include "IAudioGenerationProvider.h"
#include "Networking/HttpClient.h"
#include "Utils/UrlSafety.h"

namespace context
{
/** The ONLY place that knows bridge endpoint paths and JSON field names.
    Pure functions over a BridgeConfiguration — no I/O — so every parsing and
    error-mapping rule is unit-tested without a network. */
class BridgeApiAdapter
{
public:
    BridgeApiAdapter (BridgeConfiguration config, urls::ParsedUrl base);

    const BridgeConfiguration& config() const { return cfg; }
    const urls::ParsedUrl& base() const       { return baseUrl; }

    // URLs
    juce::String healthUrl() const;
    juce::String capabilitiesUrl() const;     // empty if the profile has none
    juce::String generateUrl() const;
    juce::String statusUrl (const juce::String& jobId) const;
    juce::String cancelUrl (const juce::String& jobId) const;
    juce::String downloadUrl (const juce::String& jobId) const;

    // Request serialisation
    juce::String serializeGenerateRequest (const GenerationRequest&, const juce::MemoryBlock* audioContextWav) const;

    // Response parsing
    ConnectionTestResult parseHealth (const HttpClient::Response&) const;
    Capabilities parseCapabilities (const HttpClient::Response&) const;
    SubmitResult parseSubmit (const HttpClient::Response&) const;
    PollResult parsePoll (const HttpClient::Response&) const;

    /** Maps a non-successful HTTP exchange to a ProviderError. */
    static ProviderError classifyFailure (const HttpClient::Response&, const juce::String& what);

    /** Extracts a short, redacted error message from a bridge JSON error body. */
    juce::String extractError (const juce::var& body) const;

    static constexpr const char* protocolName = "context-bridge-v1";

private:
    PollResult interpretStatusObject (const juce::var& body) const;
    BridgeConfiguration cfg;
    urls::ParsedUrl baseUrl;
};
} // namespace context
