#include "BridgeApiAdapter.h"
#include "Utils/Json.h"
#include "Utils/Redaction.h"

namespace context
{
namespace
{
    bool isSafeJobId (const juce::String& id)
    {
        return id.isNotEmpty() && id.length() <= 128
            && id.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:~");
    }

    ErrorCode codeFromBridgeErrorCode (const juce::String& raw)
    {
        const auto c = raw.trim().toLowerCase();
        if (c.contains ("auth") || c.contains ("session") || c.contains ("login") || c.contains ("credential"))
            return ErrorCode::UpstreamAuthExpired;
        if (c.contains ("rate") || c.contains ("quota") || c.contains ("busy"))
            return ErrorCode::RateLimited;
        if (c.contains ("invalid") || c.contains ("bad_request"))
            return ErrorCode::InvalidRequest;
        if (c.contains ("unsupported"))
            return ErrorCode::Unsupported;
        if (c.contains ("expired") && c.contains ("url"))
            return ErrorCode::DownloadExpired;
        return ErrorCode::None;
    }
}

BridgeApiAdapter::BridgeApiAdapter (BridgeConfiguration config, urls::ParsedUrl base)
    : cfg (std::move (config)), baseUrl (std::move (base))
{
}

juce::String BridgeApiAdapter::healthUrl() const       { return urls::joinPath (baseUrl.normalized, cfg.healthPath); }
juce::String BridgeApiAdapter::capabilitiesUrl() const { return cfg.capabilitiesPath.isEmpty() ? juce::String() : urls::joinPath (baseUrl.normalized, cfg.capabilitiesPath); }
juce::String BridgeApiAdapter::generateUrl() const     { return urls::joinPath (baseUrl.normalized, cfg.generatePath); }
juce::String BridgeApiAdapter::statusUrl (const juce::String& id) const   { return urls::joinPath (baseUrl.normalized, cfg.statusPath, id); }
juce::String BridgeApiAdapter::cancelUrl (const juce::String& id) const   { return cfg.cancelPath.isEmpty() ? juce::String() : urls::joinPath (baseUrl.normalized, cfg.cancelPath, id); }
juce::String BridgeApiAdapter::downloadUrl (const juce::String& id) const { return cfg.downloadPath.isEmpty() ? juce::String() : urls::joinPath (baseUrl.normalized, cfg.downloadPath, id); }

// ---------------------------------------------------------------------------

juce::String BridgeApiAdapter::serializeGenerateRequest (const GenerationRequest& r, const juce::MemoryBlock* audioContextWav) const
{
    auto* o = new juce::DynamicObject();
    const auto prompt = r.expandedPrompt.isNotEmpty() ? r.expandedPrompt : r.userPrompt;

    if (cfg.requestFormat == BridgeConfiguration::RequestFormat::ContextV1)
    {
        o->setProperty ("protocol", protocolName);
        o->setProperty ("requestId", r.requestId);
        o->setProperty ("prompt", prompt);
        o->setProperty ("userPrompt", r.userPrompt);
        o->setProperty ("type", toId (r.type));
        if (r.durationSeconds) o->setProperty ("durationSeconds", *r.durationSeconds);
        if (r.bpm)             o->setProperty ("bpm", juce::roundToInt (*r.bpm * 100.0) / 100.0);
        if (r.key.isNotEmpty())  o->setProperty ("key", r.key);
        if (r.mode.isNotEmpty()) o->setProperty ("mode", r.mode);
        if (r.instrumental)    o->setProperty ("instrumental", *r.instrumental);
        if (r.seed)            o->setProperty ("seed", *r.seed);
        if (r.variationOfRemoteId.isNotEmpty()) o->setProperty ("variationOf", r.variationOfRemoteId);
        if (r.contextAnalysis && r.contextAnalysis->valid)
            o->setProperty ("context", r.contextAnalysis->toVar());
        if (audioContextWav != nullptr && audioContextWav->getSize() > 0 && cfg.supportsAudioUpload)
        {
            auto* a = new juce::DynamicObject();
            a->setProperty ("format", "wav");
            a->setProperty ("encoding", "base64");
            a->setProperty ("data", audioContextWav->toBase64Encoding());
            o->setProperty ("audioContext", juce::var (a));
        }
    }
    else
    {
        for (auto& [src, dst] : cfg.requestFields)
        {
            juce::var value;
            if (src == "prompt")                                       value = prompt;
            else if (src == "userPrompt")                              value = r.userPrompt;
            else if (src == "type")                                    value = toId (r.type);
            else if (src == "durationSeconds" && r.durationSeconds)    value = *r.durationSeconds;
            else if (src == "bpm" && r.bpm)                            value = juce::roundToInt (*r.bpm * 100.0) / 100.0;
            else if (src == "key" && r.key.isNotEmpty())               value = r.key;
            else if (src == "mode" && r.mode.isNotEmpty())             value = r.mode;
            else if (src == "instrumental" && r.instrumental)          value = *r.instrumental;
            else if (src == "seed" && r.seed)                          value = *r.seed;
            if (! value.isVoid())
                o->setProperty (juce::Identifier (dst), value);
        }
        for (auto& nv : cfg.staticFields)
            if (! o->hasProperty (nv.name))
                o->setProperty (nv.name, nv.value);
    }
    return json::toCompactString (juce::var (o));
}

// ---------------------------------------------------------------------------

juce::String BridgeApiAdapter::extractError (const juce::var& body) const
{
    juce::String msg;
    for (auto path : { cfg.errorField, juce::String ("error.message"), juce::String ("message"), juce::String ("detail") })
    {
        if (path.isEmpty())
            continue;
        const auto v = json::getPath (body, path);
        if (v.isString() && v.toString().isNotEmpty()) { msg = v.toString(); break; }
    }
    return redaction::redact (msg.substring (0, 300));
}

ProviderError BridgeApiAdapter::classifyFailure (const HttpClient::Response& r, const juce::String& what)
{
    if (r.cancelled)
        return ProviderError::make (ErrorCode::Cancelled);
    if (! r.connected)
        return ProviderError::make (ErrorCode::BridgeOffline, what + ": " + (r.transportError.isNotEmpty() ? r.transportError : juce::String ("no response"))
                                                                     + " after " + juce::String (r.elapsedMs) + " ms");
    if (r.truncated)
        return ProviderError::make (ErrorCode::MalformedResponse, what + ": response larger than the allowed limit", r.statusCode);

    const auto detail = what + ": HTTP " + juce::String (r.statusCode) + " " + redaction::redact (r.bodyText().substring (0, 300));
    const auto parsed = json::parseUntrusted (r.bodyText(), 256 * 1024, 32);
    if (parsed.ok)
        if (auto code = json::getString (parsed.value, "errorCode"))
            if (auto mapped = codeFromBridgeErrorCode (*code); mapped != ErrorCode::None)
                return ProviderError::make (mapped, detail, r.statusCode);

    switch (r.statusCode)
    {
        case 400: case 422: return ProviderError::make (ErrorCode::InvalidRequest, detail, r.statusCode);
        case 401: case 403: return ProviderError::make (ErrorCode::UpstreamAuthExpired, detail, r.statusCode);
        case 404: case 405: return ProviderError::make (ErrorCode::EndpointMissing, detail, r.statusCode);
        case 408:           return ProviderError::make (ErrorCode::Timeout, detail, r.statusCode);
        case 429:           return ProviderError::make (ErrorCode::RateLimited, detail, r.statusCode);
        default: break;
    }
    return ProviderError::make (ErrorCode::HttpError, detail, r.statusCode);
}

ConnectionTestResult BridgeApiAdapter::parseHealth (const HttpClient::Response& r) const
{
    ConnectionTestResult t;
    if (r.cancelled)
    {
        t.status = ConnectionStatus::Disconnected;
        t.detail = "cancelled";
        return t;
    }
    if (! r.connected)
    {
        // A refused connection fails quickly (Windows retries SYNs for ~2 s); a silent host runs into the timeout.
        t.status = r.elapsedMs < 3500 ? ConnectionStatus::ConnectionRefused : ConnectionStatus::Disconnected;
        t.detail = "GET " + healthUrl() + " -> " + (r.transportError.isNotEmpty() ? r.transportError : juce::String ("no response"))
                 + " (" + juce::String (r.elapsedMs) + " ms)";
        return t;
    }
    if (r.statusCode == 404 || r.statusCode == 405)
    {
        t.status = ConnectionStatus::UnsupportedApi;
        t.detail = "Health endpoint " + cfg.healthPath + " returned HTTP " + juce::String (r.statusCode);
        return t;
    }
    if (! r.isSuccess())
    {
        t.status = ConnectionStatus::UnexpectedResponse;
        t.detail = "Health endpoint returned HTTP " + juce::String (r.statusCode);
        return t;
    }

    if (cfg.healthRequiresContextProtocol)
    {
        const auto parsed = json::parseUntrusted (r.bodyText(), 64 * 1024, 16);
        if (! parsed.ok)
        {
            t.status = ConnectionStatus::UnexpectedResponse;
            t.detail = "Health response is not JSON: " + parsed.error;
            return t;
        }
        if (json::getString (parsed.value, "protocol").value_or ("") != protocolName)
        {
            t.status = ConnectionStatus::UnsupportedApi;
            t.detail = "Bridge does not report protocol '" + juce::String (protocolName) + "'. Use a bridge profile that matches your bridge.";
            return t;
        }
        t.bridgeName = redaction::redact (json::getString (parsed.value, "name").value_or ("").substring (0, 80));
        t.bridgeVersion = redaction::redact (json::getString (parsed.value, "version").value_or ("").substring (0, 40));
    }

    t.status = ConnectionStatus::Connected;
    t.detail = "Health OK (" + juce::String (r.elapsedMs) + " ms)";
    return t;
}

Capabilities BridgeApiAdapter::parseCapabilities (const HttpClient::Response& r) const
{
    if (! r.isSuccess())
        return cfg.staticCapabilities;
    const auto parsed = json::parseUntrusted (r.bodyText(), 64 * 1024, 16);
    if (! parsed.ok || ! json::isObject (parsed.value))
        return cfg.staticCapabilities;
    auto caps = Capabilities::fromVar (parsed.value, cfg.staticCapabilities);
    // A profile without a cancel endpoint can't cancel, whatever the bridge claims.
    if (! cfg.supportsCancellation || cfg.cancelPath.isEmpty())
        caps.features.erase (Feature::Cancellation);
    if (! cfg.supportsAudioUpload)
        caps.features.erase (Feature::AudioConditioning);
    return caps;
}

SubmitResult BridgeApiAdapter::parseSubmit (const HttpClient::Response& r) const
{
    SubmitResult s;
    if (! r.isSuccess())
    {
        s.error = classifyFailure (r, "Submit");
        if (s.error.code == ErrorCode::EndpointMissing)
            s.error.userMessage = "The bridge has no generation endpoint at " + cfg.generatePath + ".";
        return s;
    }

    const auto parsed = json::parseUntrusted (r.bodyText());
    if (! parsed.ok || ! json::isObject (parsed.value))
    {
        s.error = ProviderError::make (ErrorCode::MalformedResponse, "Submit response: " + (parsed.ok ? juce::String ("not a JSON object") : parsed.error));
        return s;
    }

    if (! cfg.supportsPolling)
    {
        auto poll = interpretStatusObject (parsed.value);
        if (! poll.ok)            { s.error = poll.error; return s; }
        if (poll.state != JobState::Complete || poll.audioLocation.isEmpty())
        {
            s.error = ProviderError::make (ErrorCode::UnexpectedSchema, "Non-polling bridge did not return a finished audio URL");
            return s;
        }
        s.immediateAudioUrl = poll.audioLocation;
        s.jobId = json::getString (parsed.value, cfg.jobIdField).value_or ("sync");
        if (! isSafeJobId (s.jobId)) s.jobId = "sync";
        s.ok = true;
        return s;
    }

    const auto id = json::getString (parsed.value, cfg.jobIdField).value_or ("").trim();
    if (id.isEmpty())
    {
        const auto msg = extractError (parsed.value);
        s.error = ProviderError::make (msg.isNotEmpty() ? ErrorCode::GenerationFailed : ErrorCode::UnexpectedSchema,
                                       "Submit response has no '" + cfg.jobIdField + "'" + (msg.isNotEmpty() ? ": " + msg : juce::String()));
        if (msg.isNotEmpty())
            s.error.userMessage = "The provider rejected the request: " + msg;
        return s;
    }
    if (! isSafeJobId (id))
    {
        s.error = ProviderError::make (ErrorCode::UnexpectedSchema, "Job ID contains unexpected characters or is too long");
        return s;
    }

    s.jobId = id;
    s.ok = true;
    return s;
}

PollResult BridgeApiAdapter::interpretStatusObject (const juce::var& body) const
{
    PollResult p;
    const auto statusText = json::getString (body, cfg.statusField);
    if (! statusText && cfg.supportsPolling)
    {
        p.error = ProviderError::make (ErrorCode::UnexpectedSchema, "Status response has no '" + cfg.statusField + "' field");
        return p;
    }

    if (statusText)
    {
        const auto key = statusText->trim().toLowerCase();
        auto it = cfg.statusMap.find (key);
        p.state = it != cfg.statusMap.end() ? it->second : JobState::Generating;
        p.statusText = redaction::redact (statusText->substring (0, 60));
    }
    else
    {
        p.state = JobState::Complete;   // synchronous bridges
    }

    if (cfg.progressField.isNotEmpty())
        if (auto prog = json::getNumber (body, cfg.progressField))
            p.progress = juce::jlimit (0.0, 1.0, *prog > 1.0 ? *prog / 100.0 : *prog);

    if (p.state == JobState::Failed)
    {
        const auto msg = extractError (body);
        const auto code = codeFromBridgeErrorCode (json::getString (body, cfg.errorCodeField).value_or (""));
        p.error = ProviderError::make (code != ErrorCode::None ? code : ErrorCode::GenerationFailed,
                                       "Bridge reported failure" + (msg.isNotEmpty() ? ": " + msg : juce::String()));
        if (code == ErrorCode::None && msg.isNotEmpty())
            p.error.userMessage = "The provider couldn't generate this sound: " + msg;
        p.ok = true;     // a well-formed answer that says "failed"
        return p;
    }

    if (p.state == JobState::Complete && cfg.downloadPath.isEmpty())
    {
        const auto url = json::getString (body, cfg.audioUrlField).value_or ("").trim();
        if (url.isEmpty())
        {
            p.error = ProviderError::make (ErrorCode::UnexpectedSchema, "Job is complete but has no '" + cfg.audioUrlField + "'");
            return p;
        }
        const auto check = urls::checkDownloadUrl (url, baseUrl);
        if (! check.ok)
        {
            p.error = ProviderError::make (ErrorCode::UnexpectedSchema, check.error);
            p.error.userMessage = check.error;
            return p;
        }
        p.audioLocation = check.url;
    }

    p.ok = true;
    return p;
}

PollResult BridgeApiAdapter::parsePoll (const HttpClient::Response& r) const
{
    PollResult p;
    if (! r.isSuccess())
    {
        p.error = classifyFailure (r, "Status");
        if (r.statusCode == 404)
        {
            p.error = ProviderError::make (ErrorCode::GenerationFailed, "Status: HTTP 404 (bridge no longer knows this job; it may have restarted)", 404);
            p.error.userMessage = "The bridge lost track of this generation (it may have restarted). Try again.";
        }
        return p;
    }

    const auto parsed = json::parseUntrusted (r.bodyText());
    if (! parsed.ok || ! json::isObject (parsed.value))
    {
        p.error = ProviderError::make (ErrorCode::MalformedResponse, "Status response: " + (parsed.ok ? juce::String ("not a JSON object") : parsed.error));
        return p;
    }
    return interpretStatusObject (parsed.value);
}
} // namespace context
