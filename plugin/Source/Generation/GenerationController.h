#pragma once

#include "IAudioGenerationProvider.h"
#include "Core/SerialWorker.h"
#include <juce_events/juce_events.h>
#include <deque>

namespace context
{
/** Owns the generation job queue and runs jobs on its own worker thread.
    - one active job + up to `maxQueued` waiting
    - every provider call happens on a worker thread, never audio/message thread
    - state changes are published with sendChangeMessage() (delivered on the message thread)
    - destruction cancels everything (aborting in-flight HTTP) and joins the workers */
class GenerationController : public juce::ChangeBroadcaster
{
public:
    struct Dependencies
    {
        std::function<bool()> isLicensed;                                     // any thread
        std::function<juce::File()> generatedFolder;                          // worker thread
        std::function<juce::File()> tempFolder;                               // worker thread
        std::function<void (const GenerationResult&)> onCompleted;            // worker thread (e.g. add to history)
        std::function<std::optional<AudioConditioningPayload> (const juce::String& requestId)> makeAudioContext; // worker thread
        juce::String productName = "Context";
    };

    struct JobSnapshot
    {
        juce::String localJobId;
        juce::String remoteJobId;
        JobState state = JobState::Idle;
        double progress = -1.0;
        juce::String statusText;
        ProviderError error;
        GenerationRequest request;
        juce::uint32 startedMs = 0;
    };

    explicit GenerationController (Dependencies);
    ~GenerationController() override;

    /** Message thread. Jobs already running keep the provider they started with. */
    void setProvider (std::shared_ptr<IAudioGenerationProvider>);
    std::shared_ptr<IAudioGenerationProvider> getProvider() const;

    void setGenerationTimeoutSeconds (int seconds);

    struct EnqueueResult { bool ok = false; juce::String localJobId; ProviderError error; };
    EnqueueResult enqueue (GenerationRequest request);

    void cancelActive();
    void cancelAll();

    /** Runs a connection test on the connection worker. */
    void testConnection();
    ConnectionTestResult getConnectionResult() const;
    Capabilities getCapabilities() const;

    JobSnapshot getCurrentJob() const;              // active job, or the most recent finished one
    int numQueued() const;
    bool isBusy() const;
    std::optional<GenerationResult> getLastResult() const;

    static constexpr int maxQueued = 3;

private:
    struct Job
    {
        JobSnapshot snap;
        CancellationToken::Ptr token = CancellationToken::create();
    };
    using JobPtr = std::shared_ptr<Job>;

    void runJob (const JobPtr&);
    void runPipeline (const JobPtr&, IAudioGenerationProvider&);
    void update (const JobPtr&, std::function<void (JobSnapshot&)> change);
    void finish (const JobPtr&, JobState, ProviderError = {});

    Dependencies deps;
    std::atomic<int> timeoutSeconds { 600 };

    mutable juce::CriticalSection lock;            // guards everything below (leaf lock)
    std::shared_ptr<IAudioGenerationProvider> provider;
    std::deque<JobPtr> pending;
    JobPtr active, lastFinished;
    std::optional<GenerationResult> lastResult;
    ConnectionTestResult connection;
    CancellationToken::Ptr connectionToken;
    std::atomic<bool> shuttingDown { false };

    // Declared last so they are destroyed (joined) first, while everything above is alive.
    SerialWorker connectionWorker { "Context connection", 2 };
    SerialWorker generationWorker { "Context generation", maxQueued + 1 };
};
} // namespace context
