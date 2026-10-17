#include "LocalGenerationBridgeProvider.h"
#include "Networking/AudioFileValidator.h"
#include "Storage/Logger.h"
#include "Utils/Redaction.h"

namespace context
{
namespace { constexpr juce::int64 maxAudioContextBytes = 16 * 1024 * 1024; }  // ~90 s of 44.1 kHz stereo 16-bit

LocalGenerationBridgeProvider::LocalGenerationBridgeProvider (Options o)
    : options (std::move (o))
{
    base = urls::parseBaseUrl (options.baseUrl);
    caps = options.profile.staticCapabilities;

    if (! base.ok)
    {
        blocked = "Bridge address is not valid: " + base.error;
        blockedStatus = ConnectionStatus::InvalidAddress;
    }
    else if (base.kind != urls::HostKind::Loopback
             && ! (options.allowNonLocal && options.acknowledgedNonLocalHost.equalsIgnoreCase (base.host)))
    {
        blocked = "The bridge address points to " + urls::describe (base.kind)
                + ". Context only talks to bridges on this computer unless you allow non-local bridges in Settings.";
        blockedStatus = ConnectionStatus::NonLocalBlocked;
    }

    if (base.ok)
        adapter = std::make_unique<BridgeApiAdapter> (options.profile, base);
}

int LocalGenerationBridgeProvider::pollIntervalMs() const { return options.profile.pollIntervalMs; }

HttpClient::Request LocalGenerationBridgeProvider::makeRequest (const juce::String& url, const juce::String& method, const juce::String& body) const
{
    HttpClient::Request r;
    r.url = url;
    r.method = method;
    r.jsonBody = body;
    r.timeoutMs = options.profile.requestTimeoutMs;
    return r;
}

Capabilities LocalGenerationBridgeProvider::getCapabilities() const
{
    const juce::ScopedLock sl (capsLock);
    return caps;
}

ConnectionTestResult LocalGenerationBridgeProvider::testConnection (CancellationToken& token)
{
    ConnectionTestResult result;
    if (blocked.isNotEmpty() || adapter == nullptr)
    {
        result.status = blockedStatus;
        result.detail = blocked;
        return result;
    }

    // 1. Health
    auto healthReq = makeRequest (adapter->healthUrl(), "GET");
    healthReq.timeoutMs = juce::jmin (healthReq.timeoutMs, 5000);
    healthReq.maxResponseBytes = 64 * 1024;
    result = adapter->parseHealth (HttpClient::send (healthReq, token));
    if (result.status != ConnectionStatus::Connected)
        return result;

    // 2. Capabilities (optional endpoint)
    Capabilities found = options.profile.staticCapabilities;
    if (adapter->capabilitiesUrl().isNotEmpty())
    {
        auto capReq = makeRequest (adapter->capabilitiesUrl(), "GET");
        capReq.maxResponseBytes = 64 * 1024;
        const auto capResp = HttpClient::send (capReq, token);
        found = adapter->parseCapabilities (capResp);
        if (! capResp.isSuccess())
            result.detail << "; capabilities endpoint unavailable (HTTP " << capResp.statusCode << "), using profile defaults";
    }
    if (! options.profile.supportsCancellation || options.profile.cancelPath.isEmpty())
        found.features.erase (Feature::Cancellation);
    {
        const juce::ScopedLock sl (capsLock);
        caps = found;
    }
    result.capabilities = found;

    // 3. Does the generation endpoint exist? A GET to a POST-only route normally
    //    answers 405 (exists) or 404 (missing). We never POST here, so no job is created.
    auto probe = makeRequest (adapter->generateUrl(), "GET");
    probe.timeoutMs = 5000;
    probe.maxResponseBytes = 64 * 1024;
    const auto probeResp = HttpClient::send (probe, token);
    if (probeResp.connected && probeResp.statusCode == 404)
    {
        result.status = ConnectionStatus::GenerationEndpointMissing;
        result.detail = "GET " + options.profile.generatePath + " returned 404; the bridge has no generation endpoint at this path.";
    }
    return result;
}

SubmitResult LocalGenerationBridgeProvider::submit (const GenerationRequest& request, CancellationToken& token)
{
    SubmitResult s;
    if (blocked.isNotEmpty() || adapter == nullptr)
    {
        s.error = ProviderError::make (blockedStatus == ConnectionStatus::NonLocalBlocked ? ErrorCode::NonLocalBlocked : ErrorCode::BridgeNotConfigured, blocked);
        return s;
    }

    juce::MemoryBlock audioContext;
    const bool sendAudio = request.audioContext.has_value() && options.profile.supportsAudioUpload
                        && getCapabilities().has (Feature::AudioConditioning)
                        && request.audioContext->wavFile.existsAsFile()
                        && request.audioContext->wavFile.getSize() <= maxAudioContextBytes;
    if (request.audioContext.has_value() && ! sendAudio)
    {
        // Never drop the user's track silently: say why it can't go out.
        juce::String why = ! options.profile.supportsAudioUpload ? "this bridge profile has audio upload switched off"
                         : ! getCapabilities().has (Feature::AudioConditioning) ? "the bridge doesn't list audio_conditioning"
                         : ! request.audioContext->wavFile.existsAsFile() ? "the track audio file is missing"
                         : "the track audio is larger than 16 MB";
        s.error = ProviderError::make (ErrorCode::Unsupported, "Track context not sent: " + why);
        s.error.userMessage = "Your track wasn't sent: " + why + ".";
        return s;
    }
    if (sendAudio && ! request.audioContext->wavFile.loadFileAsData (audioContext))
    {
        s.error = ProviderError::make (ErrorCode::StorageFailed, "Could not read the track context file");
        return s;
    }

    const auto body = adapter->serializeGenerateRequest (request, sendAudio ? &audioContext : nullptr);
    auto req = makeRequest (adapter->generateUrl(), "POST", body);
    if (! options.profile.supportsPolling)
        req.timeoutMs = options.profile.syncGenerateTimeoutMs;

    logging::info ("bridge", "POST " + redaction::safeUrlForDisplay (req.url) + " (" + juce::String (body.getNumBytesAsUTF8()) + " bytes"
                             + (sendAudio ? ", includes audio context" : "") + ")");
    return adapter->parseSubmit (HttpClient::send (req, token));
}

PollResult LocalGenerationBridgeProvider::poll (const juce::String& jobId, CancellationToken& token)
{
    PollResult p;
    if (adapter == nullptr || blocked.isNotEmpty())
    {
        p.error = ProviderError::make (ErrorCode::BridgeNotConfigured, blocked);
        return p;
    }
    p = adapter->parsePoll (HttpClient::send (makeRequest (adapter->statusUrl (jobId), "GET"), token));
    if (p.ok && p.state == JobState::Complete && options.profile.downloadPath.isNotEmpty())
        p.audioLocation = adapter->downloadUrl (jobId);
    return p;
}

bool LocalGenerationBridgeProvider::cancel (const juce::String& jobId, CancellationToken& token)
{
    if (adapter == nullptr || ! options.profile.supportsCancellation || adapter->cancelUrl (jobId).isEmpty())
        return false;
    auto req = makeRequest (adapter->cancelUrl (jobId), "POST", "{}");
    req.timeoutMs = 3000;
    return HttpClient::send (req, token).isSuccess();
}

DownloadResult LocalGenerationBridgeProvider::download (const PollResult& completed, const juce::File& destination, CancellationToken& token)
{
    DownloadResult d;
    if (completed.audioLocation.isEmpty())
    {
        d.error = ProviderError::make (ErrorCode::UnexpectedSchema, "No audio location to download");
        return d;
    }

    const auto check = urls::checkDownloadUrl (completed.audioLocation, base);
    if (! check.ok)
    {
        d.error = ProviderError::make (ErrorCode::DownloadFailed, check.error);
        return d;
    }

    logging::info ("bridge", "Downloading " + redaction::safeUrlForDisplay (check.url));
    const auto r = HttpClient::downloadToFile (check.url, destination, AudioFileValidator::maxFileBytes, 120000, token);

    if (r.cancelled)             { d.error = ProviderError::make (ErrorCode::Cancelled); return d; }
    if (! r.connected)           { d.error = ProviderError::make (ErrorCode::DownloadFailed, "Download: " + r.transportError); d.error.retryable = true; return d; }
    if (r.statusCode == 403 || r.statusCode == 404 || r.statusCode == 410)
                                 { d.error = ProviderError::make (ErrorCode::DownloadExpired, "Download: HTTP " + juce::String (r.statusCode), r.statusCode); return d; }
    if (r.statusCode < 200 || r.statusCode >= 300)
                                 { d.error = ProviderError::make (ErrorCode::DownloadFailed, "Download: HTTP " + juce::String (r.statusCode), r.statusCode);
                                   d.error.retryable = r.statusCode >= 500; return d; }
    if (r.tooLarge)              { d.error = ProviderError::make (ErrorCode::DownloadTooLarge, "Download exceeded 100 MB"); return d; }
    if (r.writeFailed)           { d.error = ProviderError::make (ErrorCode::StorageFailed, "Could not write " + destination.getFullPathName()); return d; }
    if (r.transportError.isNotEmpty())
                                 { d.error = ProviderError::make (ErrorCode::DownloadFailed, r.transportError); d.error.retryable = true; return d; }

    d.bytes = r.bytes;
    d.ok = true;
    return d;
}
} // namespace context
