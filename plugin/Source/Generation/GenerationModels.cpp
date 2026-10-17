#include "GenerationModels.h"
#include "Utils/Json.h"

namespace context
{
juce::String toString (JobState s)
{
    switch (s)
    {
        case JobState::Idle:        return "Idle";
        case JobState::Queued:      return "Queued";
        case JobState::Submitting:  return "Submitting";
        case JobState::Generating:  return "Generating";
        case JobState::Polling:     return "Polling";
        case JobState::Downloading: return "Downloading";
        case JobState::Processing:  return "Processing";
        case JobState::Complete:    return "Complete";
        case JobState::Cancelled:   return "Cancelled";
        case JobState::Failed:      return "Failed";
    }
    return "Idle";
}

// ---------------------------------------------------------------------------

juce::String toId (Feature f)
{
    switch (f)
    {
        case Feature::FullSong:          return "full_song";
        case Feature::AudioConditioning: return "audio_conditioning";
        case Feature::Continuation:      return "continuation";
        case Feature::Variation:         return "variation";
        case Feature::Cover:             return "cover";
        case Feature::StemGeneration:    return "stem_generation";
        case Feature::Cancellation:      return "cancellation";
    }
    return {};
}

std::optional<Feature> featureFromId (const juce::String& id)
{
    for (auto f : { Feature::FullSong, Feature::AudioConditioning, Feature::Continuation, Feature::Variation,
                    Feature::Cover, Feature::StemGeneration, Feature::Cancellation })
        if (toId (f) == id.trim().toLowerCase())
            return f;
    return std::nullopt;
}

Capabilities Capabilities::allTypes()
{
    Capabilities c;
    for (auto t : allGenerationTypes)
        c.types.insert (t);
    return c;
}

juce::var Capabilities::toVar() const
{
    auto* o = new juce::DynamicObject();
    juce::Array<juce::var> t, f;
    for (auto x : types)    t.add (toId (x));
    for (auto x : features) f.add (toId (x));
    o->setProperty ("generationTypes", t);
    o->setProperty ("features", f);
    o->setProperty ("maxDurationSeconds", maxDurationSeconds);
    return juce::var (o);
}

Capabilities Capabilities::fromVar (const juce::var& v, const Capabilities& fallback)
{
    Capabilities c = fallback;
    if (auto* arr = json::getPath (v, "generationTypes").getArray())
    {
        c.types.clear();
        for (auto& x : *arr)
            if (auto t = generationTypeFromId (x.toString()))
                c.types.insert (*t);
    }
    if (auto* arr = json::getPath (v, "features").getArray())
    {
        c.features.clear();
        for (auto& x : *arr)
            if (auto f = featureFromId (x.toString()))
                c.features.insert (*f);
    }
    if (auto m = json::getNumber (v, "maxDurationSeconds"))
        c.maxDurationSeconds = juce::jlimit (1, 600, (int) *m);
    return c;
}

// ---------------------------------------------------------------------------

juce::String toId (ErrorCode c)
{
    switch (c)
    {
        case ErrorCode::None:                return "none";
        case ErrorCode::NotLicensed:         return "not_licensed";
        case ErrorCode::QueueFull:           return "queue_full";
        case ErrorCode::InvalidRequest:      return "invalid_request";
        case ErrorCode::BridgeNotConfigured: return "bridge_not_configured";
        case ErrorCode::NonLocalBlocked:     return "non_local_blocked";
        case ErrorCode::BridgeOffline:       return "bridge_offline";
        case ErrorCode::Timeout:             return "timeout";
        case ErrorCode::HttpError:           return "http_error";
        case ErrorCode::MalformedResponse:   return "malformed_response";
        case ErrorCode::UnexpectedSchema:    return "unexpected_schema";
        case ErrorCode::UnsupportedApi:      return "unsupported_api";
        case ErrorCode::EndpointMissing:     return "endpoint_missing";
        case ErrorCode::UpstreamAuthExpired: return "upstream_auth_expired";
        case ErrorCode::RateLimited:         return "rate_limited";
        case ErrorCode::GenerationFailed:    return "generation_failed";
        case ErrorCode::Cancelled:           return "cancelled";
        case ErrorCode::DownloadFailed:      return "download_failed";
        case ErrorCode::DownloadExpired:     return "download_expired";
        case ErrorCode::DownloadTooLarge:    return "download_too_large";
        case ErrorCode::DecodeFailed:        return "decode_failed";
        case ErrorCode::Unsupported:         return "unsupported";
        case ErrorCode::StorageFailed:       return "storage_failed";
        case ErrorCode::Internal:            return "internal";
    }
    return "internal";
}

juce::String ProviderError::defaultUserMessage (ErrorCode c)
{
    switch (c)
    {
        case ErrorCode::None:                return {};
        case ErrorCode::NotLicensed:         return "Activate Context to generate sounds.";
        case ErrorCode::QueueFull:           return "Already generating. Wait for the current sound to finish.";
        case ErrorCode::InvalidRequest:      return "That request can't be generated. Check the prompt and options.";
        case ErrorCode::BridgeNotConfigured: return "No generation bridge is configured. Open Settings to set one up.";
        case ErrorCode::NonLocalBlocked:     return "The bridge address is not on this computer. Allow non-local bridges in Settings to use it.";
        case ErrorCode::BridgeOffline:       return "Local generation bridge is offline.";
        case ErrorCode::Timeout:             return "Generation timed out.";
        case ErrorCode::HttpError:           return "The generation bridge returned an error.";
        case ErrorCode::MalformedResponse:   return "Generation provider returned an unexpected response.";
        case ErrorCode::UnexpectedSchema:    return "Generation provider returned an unexpected response.";
        case ErrorCode::UnsupportedApi:      return "The bridge doesn't speak an API this profile understands.";
        case ErrorCode::EndpointMissing:     return "The bridge has no generation endpoint at the configured path.";
        case ErrorCode::UpstreamAuthExpired: return "Your local bridge reports its upstream sign-in has expired. Re-authenticate in the bridge, then try again.";
        case ErrorCode::RateLimited:         return "The provider is busy or rate-limited. Try again in a moment.";
        case ErrorCode::GenerationFailed:    return "The provider couldn't generate this sound.";
        case ErrorCode::Cancelled:           return "Generation cancelled.";
        case ErrorCode::DownloadFailed:      return "The generated audio couldn't be downloaded.";
        case ErrorCode::DownloadExpired:     return "The download link for this sound has expired.";
        case ErrorCode::DownloadTooLarge:    return "The generated file is larger than the 100 MB limit.";
        case ErrorCode::DecodeFailed:        return "Generated audio could not be decoded.";
        case ErrorCode::Unsupported:         return "This provider doesn't support that feature.";
        case ErrorCode::StorageFailed:       return "Context couldn't save the generated file. Check free disk space and folder permissions.";
        case ErrorCode::Internal:            return "Something went wrong inside Context.";
    }
    return "Something went wrong inside Context.";
}

ProviderError ProviderError::make (ErrorCode code, juce::String detail, int httpStatus)
{
    ProviderError e;
    e.code = code;
    e.userMessage = defaultUserMessage (code);
    e.technicalDetail = std::move (detail);
    e.httpStatus = httpStatus;
    e.retryable = code == ErrorCode::BridgeOffline || code == ErrorCode::Timeout || code == ErrorCode::RateLimited
               || (code == ErrorCode::HttpError && httpStatus >= 500);
    return e;
}

juce::String toDisplayString (ConnectionStatus s)
{
    switch (s)
    {
        case ConnectionStatus::Unknown:                   return "Not tested";
        case ConnectionStatus::Testing:                   return "Testing...";
        case ConnectionStatus::Connected:                 return "Connected";
        case ConnectionStatus::Disconnected:              return "Disconnected";
        case ConnectionStatus::ConnectionRefused:         return "Connection Refused";
        case ConnectionStatus::UnexpectedResponse:        return "Unexpected Response";
        case ConnectionStatus::UnsupportedApi:            return "Unsupported API";
        case ConnectionStatus::GenerationEndpointMissing: return "Generation Endpoint Missing";
        case ConnectionStatus::NonLocalBlocked:           return "Blocked: not a local address";
        case ConnectionStatus::InvalidAddress:            return "Invalid Address";
        case ConnectionStatus::MockReady:                 return "Mock Provider (offline test mode)";
    }
    return "Not tested";
}

// ---------------------------------------------------------------------------

juce::var GenerationRequest::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("requestId", requestId);
    o->setProperty ("prompt", userPrompt);
    o->setProperty ("expandedPrompt", expandedPrompt);
    o->setProperty ("type", toId (type));
    if (durationSeconds) o->setProperty ("durationSeconds", *durationSeconds);
    if (bpm)             o->setProperty ("bpm", *bpm);
    if (key.isNotEmpty())  o->setProperty ("key", key);
    if (mode.isNotEmpty()) o->setProperty ("mode", mode);
    if (keyConfidence)   o->setProperty ("keyConfidence", *keyConfidence);
    if (instrumental)    o->setProperty ("instrumental", *instrumental);
    if (seed)            o->setProperty ("seed", *seed);
    if (variationOfGenerationId.isNotEmpty()) o->setProperty ("variationOf", variationOfGenerationId);
    if (variationOfRemoteId.isNotEmpty())     o->setProperty ("variationOfRemoteId", variationOfRemoteId);
    if (contextAnalysis && contextAnalysis->valid) o->setProperty ("context", contextAnalysis->toVar());
    return juce::var (o);
}

GenerationRequest GenerationRequest::fromVar (const juce::var& v)
{
    GenerationRequest r;
    r.requestId      = json::getString (v, "requestId").value_or ("");
    r.userPrompt     = json::getString (v, "prompt").value_or ("");
    r.expandedPrompt = json::getString (v, "expandedPrompt").value_or (r.userPrompt);
    r.type           = generationTypeFromId (json::getString (v, "type").value_or ("")).value_or (GenerationType::OneShot);
    if (auto d = json::getNumber (v, "durationSeconds")) r.durationSeconds = *d;
    if (auto d = json::getNumber (v, "bpm"))             r.bpm = *d;
    r.key  = json::getString (v, "key").value_or ("");
    r.mode = json::getString (v, "mode").value_or ("");
    if (auto d = json::getNumber (v, "keyConfidence"))   r.keyConfidence = *d;
    if (auto b = json::getBool (v, "instrumental"))      r.instrumental = *b;
    if (auto d = json::getNumber (v, "seed"))            r.seed = (juce::int64) *d;
    r.variationOfGenerationId = json::getString (v, "variationOf").value_or ("");
    r.variationOfRemoteId     = json::getString (v, "variationOfRemoteId").value_or ("");
    return r;
}

juce::String GenerationRequest::validate() const
{
    if (userPrompt.trim().isEmpty())
        return "Type what sound you want first.";
    if (userPrompt.length() > 1000)
        return "Prompt is too long (1000 characters max).";
    if (expandedPrompt.length() > 4000)
        return "Expanded prompt is too long.";
    if (bpm && (*bpm < 20.0 || *bpm > 999.0 || ! std::isfinite (*bpm)))
        return "BPM is out of range.";
    if (durationSeconds && (*durationSeconds <= 0.0 || *durationSeconds > 600.0 || ! std::isfinite (*durationSeconds)))
        return "Duration is out of range.";
    return {};
}

// ---------------------------------------------------------------------------

juce::var GenerationResult::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("generationId", generationId);
    o->setProperty ("providerId", providerId);
    o->setProperty ("remoteJobId", remoteJobId);
    o->setProperty ("status", toString (status));
    o->setProperty ("prompt", prompt);
    o->setProperty ("expandedPrompt", expandedPrompt);
    o->setProperty ("timestamp", creationTimestamp.toMilliseconds());
    o->setProperty ("remoteUrl", remoteUrl);
    o->setProperty ("localFile", localFile.getFullPathName());
    o->setProperty ("duration", durationSeconds);
    o->setProperty ("sampleRate", sampleRate);
    o->setProperty ("channels", channelCount);
    o->setProperty ("type", toId (type));
    if (bpm) o->setProperty ("bpm", *bpm);
    o->setProperty ("key", key);
    o->setProperty ("mode", mode);
    o->setProperty ("favorite", favorite);
    o->setProperty ("lastUsedMs", lastUsedMs);
    if (errorCode != ErrorCode::None)
    {
        o->setProperty ("errorCode", toId (errorCode));
        o->setProperty ("errorMessage", errorMessage);
    }
    auto* meta = new juce::DynamicObject();
    for (const auto& nv : metadata)
        meta->setProperty (nv.name, nv.value);
    o->setProperty ("metadata", juce::var (meta));
    return juce::var (o);
}

std::optional<GenerationResult> GenerationResult::fromVar (const juce::var& v)
{
    if (! json::isObject (v))
        return std::nullopt;

    GenerationResult r;
    r.generationId = json::getString (v, "generationId").value_or ("");
    const auto file = json::getString (v, "localFile").value_or ("");
    if (r.generationId.isEmpty() || r.generationId.length() > 64 || file.isEmpty() || ! juce::File::isAbsolutePath (file))
        return std::nullopt;

    r.localFile         = juce::File (file);
    r.providerId        = json::getString (v, "providerId").value_or ("");
    r.remoteJobId       = json::getString (v, "remoteJobId").value_or ("");
    r.status            = JobState::Complete;
    r.prompt            = json::getString (v, "prompt").value_or ("").substring (0, 1000);
    r.expandedPrompt    = json::getString (v, "expandedPrompt").value_or ("").substring (0, 4000);
    r.creationTimestamp = juce::Time ((juce::int64) json::getNumber (v, "timestamp").value_or (0.0));
    r.remoteUrl         = json::getString (v, "remoteUrl").value_or ("");
    r.durationSeconds   = json::getNumber (v, "duration").value_or (0.0);
    r.sampleRate        = json::getNumber (v, "sampleRate").value_or (0.0);
    r.channelCount      = (int) json::getNumber (v, "channels").value_or (0.0);
    r.type              = generationTypeFromId (json::getString (v, "type").value_or ("")).value_or (GenerationType::OneShot);
    if (auto b = json::getNumber (v, "bpm")) r.bpm = *b;
    r.key               = json::getString (v, "key").value_or ("");
    r.mode              = json::getString (v, "mode").value_or ("");
    r.favorite          = json::getBool (v, "favorite").value_or (false);
    r.lastUsedMs        = (juce::int64) json::getNumber (v, "lastUsedMs").value_or (0.0);
    if (auto* meta = json::getPath (v, "metadata").getDynamicObject())
        r.metadata = meta->getProperties();
    return r;
}
} // namespace context
