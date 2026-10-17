#pragma once

#include "IAudioGenerationProvider.h"
#include <map>

namespace context
{
/** Fully offline provider for testing the whole product without any AI service.
    Simulates connection, latency, progress, polling, cancellation and failures,
    and produces synthetic WAV files (see SynthAudio).

    Test triggers in the prompt:
      #fail      the job fails
      #timeout   the job never finishes (exercises the generation timeout)
      #slow      takes ~4x longer */
class MockAudioGenerationProvider final : public IAudioGenerationProvider
{
public:
    struct Options
    {
        double latencyScale = 1.0;     // 1.0 = 2.5-5 s per generation
        int failurePercent = 0;        // random failure rate
        juce::int64 randomSeed = 0;    // 0 = time based
    };

    explicit MockAudioGenerationProvider (Options);

    juce::String id() const override          { return "mock"; }
    juce::String displayName() const override { return "Mock Provider (offline test)"; }

    ConnectionTestResult testConnection (CancellationToken&) override;
    Capabilities getCapabilities() const override;
    SubmitResult submit (const GenerationRequest&, CancellationToken&) override;
    PollResult poll (const juce::String& jobId, CancellationToken&) override;
    bool cancel (const juce::String& jobId, CancellationToken&) override;
    DownloadResult download (const PollResult&, const juce::File& destination, CancellationToken&) override;
    int pollIntervalMs() const override;

private:
    struct Job
    {
        GenerationRequest request;
        juce::uint32 startMs = 0;
        int durationMs = 3000;
        bool willFail = false, never = false, cancelled = false;
        juce::int64 seed = 0;
    };

    Options options;
    juce::CriticalSection lock;
    std::map<juce::String, Job> jobs;
    juce::Random rng;
};
} // namespace context
