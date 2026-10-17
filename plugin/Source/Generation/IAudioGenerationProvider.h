#pragma once

#include "GenerationModels.h"
#include "Utils/CancellationToken.h"

namespace context
{
struct SubmitResult
{
    bool ok = false;
    juce::String jobId;                  // provider-side job ID
    juce::String immediateAudioUrl;      // set by providers that return audio without polling
    ProviderError error;
};

struct PollResult
{
    bool ok = false;
    JobState state = JobState::Generating;   // Queued / Generating / Complete / Failed / Cancelled
    double progress = -1.0;                  // 0..1, or -1 when unknown
    juce::String audioLocation;              // URL (bridge) or local path (mock), valid when Complete
    juce::String statusText;                 // optional provider message
    ProviderError error;
};

struct DownloadResult
{
    bool ok = false;
    juce::int64 bytes = 0;
    ProviderError error;
};

/** A source of generated audio. All methods block and are called ONLY from
    background worker threads (never the audio or message thread). Each call takes
    a CancellationToken that the implementation must honour promptly.
    Implementations must be thread-safe: a connection test may run on one worker
    while a generation polls on another. */
class IAudioGenerationProvider
{
public:
    virtual ~IAudioGenerationProvider() = default;

    virtual juce::String id() const = 0;            // "mock", "bridge"
    virtual juce::String displayName() const = 0;

    virtual ConnectionTestResult testConnection (CancellationToken&) = 0;

    /** Last known capabilities (updated by testConnection). Cheap; no I/O. */
    virtual Capabilities getCapabilities() const = 0;

    virtual SubmitResult submit (const GenerationRequest&, CancellationToken&) = 0;
    virtual PollResult poll (const juce::String& jobId, CancellationToken&) = 0;

    /** Best effort. Returns false if the provider can't cancel remotely. */
    virtual bool cancel (const juce::String& jobId, CancellationToken&) = 0;

    /** Writes the finished audio to `destination` (a temp file). Size limits apply. */
    virtual DownloadResult download (const PollResult& completed, const juce::File& destination, CancellationToken&) = 0;

    virtual int pollIntervalMs() const { return 2000; }
};
} // namespace context
