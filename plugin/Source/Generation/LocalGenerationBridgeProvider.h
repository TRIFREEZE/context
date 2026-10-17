#pragma once

#include "IAudioGenerationProvider.h"
#include "BridgeApiAdapter.h"

namespace context
{
/** Talks HTTP/JSON to a user-run generation bridge (default http://127.0.0.1:3000).
    Knows nothing about how the bridge authenticates upstream: it never sends or
    stores provider credentials, cookies or tokens. All endpoint/field knowledge
    lives in BridgeApiAdapter + BridgeConfiguration. */
class LocalGenerationBridgeProvider final : public IAudioGenerationProvider
{
public:
    struct Options
    {
        juce::String baseUrl = "http://127.0.0.1:3000";
        BridgeConfiguration profile = BridgeConfiguration::contextV1();
        bool allowNonLocal = false;
        juce::String acknowledgedNonLocalHost;     // must equal the URL host for non-local use
    };

    explicit LocalGenerationBridgeProvider (Options);

    juce::String id() const override          { return "bridge"; }
    juce::String displayName() const override { return "Local Generation Bridge"; }

    ConnectionTestResult testConnection (CancellationToken&) override;
    Capabilities getCapabilities() const override;
    SubmitResult submit (const GenerationRequest&, CancellationToken&) override;
    PollResult poll (const juce::String& jobId, CancellationToken&) override;
    bool cancel (const juce::String& jobId, CancellationToken&) override;
    DownloadResult download (const PollResult&, const juce::File& destination, CancellationToken&) override;
    int pollIntervalMs() const override;

    /** Non-empty if the configured address may not be used (invalid or non-local without consent). */
    juce::String blockedReason() const { return blocked; }

private:
    HttpClient::Request makeRequest (const juce::String& url, const juce::String& method, const juce::String& body = {}) const;

    Options options;
    urls::ParsedUrl base;
    std::unique_ptr<BridgeApiAdapter> adapter;
    juce::String blocked;
    ConnectionStatus blockedStatus = ConnectionStatus::Unknown;

    mutable juce::CriticalSection capsLock;
    Capabilities caps;
};
} // namespace context
