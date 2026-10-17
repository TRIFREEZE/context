#include "MockAudioGenerationProvider.h"
#include "SynthAudio.h"

namespace context
{
MockAudioGenerationProvider::MockAudioGenerationProvider (Options o)
    : options (o), rng (o.randomSeed != 0 ? o.randomSeed : juce::Time::currentTimeMillis())
{
}

int MockAudioGenerationProvider::pollIntervalMs() const
{
    return juce::jlimit (50, 1000, (int) (500.0 * options.latencyScale));
}

ConnectionTestResult MockAudioGenerationProvider::testConnection (CancellationToken& token)
{
    token.sleepOrCancelled ((int) (150 * options.latencyScale));
    ConnectionTestResult r;
    r.status = ConnectionStatus::MockReady;
    r.bridgeName = "Mock Provider";
    r.bridgeVersion = "1";
    r.detail = "Offline mock provider: synthetic test audio, no network";
    r.capabilities = getCapabilities();
    return r;
}

Capabilities MockAudioGenerationProvider::getCapabilities() const
{
    auto c = Capabilities::allTypes();
    c.features = { Feature::Variation, Feature::Cancellation };
    c.maxDurationSeconds = 30;
    return c;
}

SubmitResult MockAudioGenerationProvider::submit (const GenerationRequest& request, CancellationToken& token)
{
    SubmitResult s;
    if (token.sleepOrCancelled ((int) (200 * options.latencyScale)))
    {
        s.error = ProviderError::make (ErrorCode::Cancelled);
        return s;
    }

    const auto prompt = request.userPrompt.toLowerCase();
    Job job;
    job.request = request;
    job.startMs = juce::Time::getMillisecondCounter();

    const juce::ScopedLock sl (lock);
    job.durationMs = (int) ((2500 + rng.nextInt (2500)) * options.latencyScale * (prompt.contains ("#slow") ? 4.0 : 1.0));
    job.willFail = prompt.contains ("#fail") || rng.nextInt (100) < options.failurePercent;
    job.never = prompt.contains ("#timeout");

    // Variations reuse the source seed with a small offset so they sound related.
    if (request.seed)
        job.seed = *request.seed;
    else
        job.seed = rng.nextInt64();

    s.jobId = "mock-" + juce::Uuid().toDashedString();
    jobs[s.jobId] = job;
    s.ok = true;
    return s;
}

PollResult MockAudioGenerationProvider::poll (const juce::String& jobId, CancellationToken&)
{
    PollResult p;
    const juce::ScopedLock sl (lock);
    auto it = jobs.find (jobId);
    if (it == jobs.end())
    {
        p.error = ProviderError::make (ErrorCode::GenerationFailed, "Unknown mock job " + jobId);
        return p;
    }

    auto& job = it->second;
    p.ok = true;
    if (job.cancelled)
    {
        p.state = JobState::Cancelled;
        return p;
    }

    const auto elapsed = (double) (juce::Time::getMillisecondCounter() - job.startMs);
    const double progress = job.never ? juce::jmin (0.95, elapsed / (double) job.durationMs) : elapsed / (double) job.durationMs;

    if (! job.never && job.willFail && progress > 0.6)
    {
        p.state = JobState::Failed;
        p.error = ProviderError::make (ErrorCode::GenerationFailed, "Mock provider simulated a failure");
        p.error.userMessage = "The mock provider simulated a generation failure (test).";
        jobs.erase (it);
        return p;
    }

    if (! job.never && progress >= 1.0)
    {
        p.state = JobState::Complete;
        p.progress = 1.0;
        p.audioLocation = "mock://" + jobId;
        return p;
    }

    p.state = progress < 0.1 ? JobState::Queued : JobState::Generating;
    p.progress = juce::jlimit (0.0, 0.99, progress);
    p.statusText = progress < 0.1 ? "queued" : "synthesizing";
    return p;
}

bool MockAudioGenerationProvider::cancel (const juce::String& jobId, CancellationToken&)
{
    const juce::ScopedLock sl (lock);
    auto it = jobs.find (jobId);
    if (it == jobs.end())
        return false;
    it->second.cancelled = true;
    return true;
}

DownloadResult MockAudioGenerationProvider::download (const PollResult& completed, const juce::File& destination, CancellationToken& token)
{
    DownloadResult d;
    const auto jobId = completed.audioLocation.fromFirstOccurrenceOf ("mock://", false, false);

    Job job;
    {
        const juce::ScopedLock sl (lock);
        auto it = jobs.find (jobId);
        if (it == jobs.end())
        {
            d.error = ProviderError::make (ErrorCode::DownloadExpired, "Mock job no longer exists");
            return d;
        }
        job = it->second;
        jobs.erase (it);
    }

    if (token.isCancelled())
    {
        d.error = ProviderError::make (ErrorCode::Cancelled);
        return d;
    }

    const auto rendered = synth::render (job.request, job.seed, 44100.0);
    if (! synth::writeWav (rendered.audio, rendered.sampleRate, destination))
    {
        d.error = ProviderError::make (ErrorCode::StorageFailed, "Could not write " + destination.getFullPathName());
        return d;
    }
    d.bytes = destination.getSize();
    d.ok = true;
    return d;
}
} // namespace context
