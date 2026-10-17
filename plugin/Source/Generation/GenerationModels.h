#pragma once

#include "Core/Types.h"
#include "Analysis/AnalysisTypes.h"
#include <set>

namespace context
{
// ---------------------------------------------------------------------------
// Job states
// ---------------------------------------------------------------------------
enum class JobState { Idle, Queued, Submitting, Generating, Polling, Downloading, Processing, Complete, Cancelled, Failed };

juce::String toString (JobState);
inline bool isTerminal (JobState s) { return s == JobState::Complete || s == JobState::Cancelled || s == JobState::Failed; }
inline bool isActive (JobState s)   { return s != JobState::Idle && ! isTerminal (s); }

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------
enum class Feature { FullSong, AudioConditioning, Continuation, Variation, Cover, StemGeneration, Cancellation };

struct Capabilities
{
    std::set<GenerationType> types;          // which generation types the provider accepts
    std::set<Feature> features;
    int maxDurationSeconds = 30;

    bool supports (GenerationType t) const { return types.count (t) > 0; }
    bool has (Feature f) const             { return features.count (f) > 0; }

    static Capabilities allTypes();
    juce::var toVar() const;
    static Capabilities fromVar (const juce::var&, const Capabilities& fallback);
};

juce::String toId (Feature);
std::optional<Feature> featureFromId (const juce::String&);

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------
enum class ErrorCode
{
    None, NotLicensed, QueueFull, InvalidRequest, BridgeNotConfigured, NonLocalBlocked,
    BridgeOffline, Timeout, HttpError, MalformedResponse, UnexpectedSchema, UnsupportedApi,
    EndpointMissing, UpstreamAuthExpired, RateLimited, GenerationFailed, Cancelled,
    DownloadFailed, DownloadExpired, DownloadTooLarge, DecodeFailed, Unsupported, StorageFailed, Internal
};

juce::String toId (ErrorCode);

struct ProviderError
{
    ErrorCode code = ErrorCode::None;
    juce::String userMessage;        // short, friendly, shown in the UI
    juce::String technicalDetail;    // shown behind "Details"; already redacted by the producer
    int httpStatus = 0;
    bool retryable = false;

    bool isError() const { return code != ErrorCode::None; }
    static ProviderError make (ErrorCode code, juce::String detail = {}, int httpStatus = 0);
    static juce::String defaultUserMessage (ErrorCode);
};

// ---------------------------------------------------------------------------
// Connection test
// ---------------------------------------------------------------------------
enum class ConnectionStatus { Unknown, Testing, Connected, Disconnected, ConnectionRefused, UnexpectedResponse,
                              UnsupportedApi, GenerationEndpointMissing, NonLocalBlocked, InvalidAddress, MockReady };

juce::String toDisplayString (ConnectionStatus);

struct ConnectionTestResult
{
    ConnectionStatus status = ConnectionStatus::Unknown;
    juce::String detail;                      // technical detail for diagnostics
    juce::String bridgeName, bridgeVersion;
    std::optional<Capabilities> capabilities;
    bool ok() const { return status == ConnectionStatus::Connected || status == ConnectionStatus::MockReady; }
};

// ---------------------------------------------------------------------------
// Request / result
// ---------------------------------------------------------------------------
/** Optional recent-audio snapshot for providers that accept audio conditioning.
    Only ever created when the user explicitly enabled sending audio context. */
struct AudioConditioningPayload
{
    juce::File wavFile;          // temporary file in Temp/, deleted after submit
    double seconds = 0.0;
    double sampleRate = 0.0;
};

struct GenerationRequest
{
    juce::String requestId;                 // local UUID
    juce::String userPrompt;
    juce::String expandedPrompt;            // == userPrompt when expansion is off
    GenerationType type = GenerationType::OneShot;
    std::optional<double> durationSeconds;
    std::optional<double> bpm;
    juce::String key;                       // "F#" or empty
    juce::String mode;                      // "minor"/"major" or empty
    std::optional<double> keyConfidence;
    std::optional<bool> instrumental;
    std::optional<juce::int64> seed;
    juce::String variationOfGenerationId;   // local id of the source result
    juce::String variationOfRemoteId;       // provider job id of the source, if known
    std::optional<AudioConditioningPayload> audioContext;
    juce::NamedValueSet providerParameters;
    std::optional<TrackAnalysis> contextAnalysis;

    juce::var toVar() const;                // for history / diagnostics (no audio)
    static GenerationRequest fromVar (const juce::var&);
    juce::String validate() const;          // empty if OK
};

struct GenerationResult
{
    juce::String generationId;
    juce::String providerId;
    juce::String remoteJobId;
    JobState status = JobState::Idle;
    juce::String prompt, expandedPrompt;
    juce::Time creationTimestamp;
    juce::String remoteUrl;                 // redacted for display; never contains credentials
    juce::File localFile;
    double durationSeconds = 0.0;
    double sampleRate = 0.0;
    int channelCount = 0;
    GenerationType type = GenerationType::OneShot;
    std::optional<double> bpm;
    juce::String key, mode;
    bool favorite = false;
    juce::int64 lastUsedMs = 0;
    juce::NamedValueSet metadata;
    ErrorCode errorCode = ErrorCode::None;
    juce::String errorMessage;

    juce::var toVar() const;
    static std::optional<GenerationResult> fromVar (const juce::var&);
};
} // namespace context
