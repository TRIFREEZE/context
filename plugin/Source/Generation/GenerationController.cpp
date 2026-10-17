#include "GenerationController.h"
#include "Networking/AudioFileValidator.h"
#include "Storage/Logger.h"
#include "Utils/FilenameSanitizer.h"
#include "Utils/Redaction.h"

namespace context
{
namespace
{
    struct TempFileGuard
    {
        juce::File file;
        ~TempFileGuard() { if (file != juce::File()) file.deleteFile(); }
    };
}

GenerationController::GenerationController (Dependencies d)
    : deps (std::move (d))
{
    auto reportException = [] (const juce::String& what) { logging::error ("generation", "Worker task threw: " + what); };
    generationWorker.onTaskException = reportException;
    connectionWorker.onTaskException = reportException;
}

GenerationController::~GenerationController()
{
    shuttingDown.store (true);
    cancelAll();
    {
        const juce::ScopedLock sl (lock);
        if (connectionToken != nullptr)
            connectionToken->cancel();
    }
    generationWorker.clearPending();
    connectionWorker.clearPending();
    // Workers are joined by their destructors (members declared last).
}

void GenerationController::setProvider (std::shared_ptr<IAudioGenerationProvider> p)
{
    {
        const juce::ScopedLock sl (lock);
        provider = std::move (p);
        connection = {};
        if (provider != nullptr)
            connection.capabilities = provider->getCapabilities();
    }
    sendChangeMessage();
}

std::shared_ptr<IAudioGenerationProvider> GenerationController::getProvider() const
{
    const juce::ScopedLock sl (lock);
    return provider;
}

void GenerationController::setGenerationTimeoutSeconds (int s) { timeoutSeconds.store (juce::jlimit (2, 3600, s)); }

ConnectionTestResult GenerationController::getConnectionResult() const
{
    const juce::ScopedLock sl (lock);
    return connection;
}

Capabilities GenerationController::getCapabilities() const
{
    const juce::ScopedLock sl (lock);
    if (connection.capabilities)
        return *connection.capabilities;
    return provider != nullptr ? provider->getCapabilities() : Capabilities{};
}

GenerationController::JobSnapshot GenerationController::getCurrentJob() const
{
    const juce::ScopedLock sl (lock);
    if (active != nullptr)       return active->snap;
    if (! pending.empty())       return pending.front()->snap;
    if (lastFinished != nullptr) return lastFinished->snap;
    return {};
}

int GenerationController::numQueued() const
{
    const juce::ScopedLock sl (lock);
    return (int) pending.size();
}

bool GenerationController::isBusy() const
{
    const juce::ScopedLock sl (lock);
    return active != nullptr || ! pending.empty();
}

std::optional<GenerationResult> GenerationController::getLastResult() const
{
    const juce::ScopedLock sl (lock);
    return lastResult;
}

// ---------------------------------------------------------------------------

void GenerationController::testConnection()
{
    std::shared_ptr<IAudioGenerationProvider> p;
    CancellationToken::Ptr token = CancellationToken::create();
    {
        const juce::ScopedLock sl (lock);
        p = provider;
        if (connectionToken != nullptr)
            connectionToken->cancel();           // supersede any running test
        connectionToken = token;
        connection.status = ConnectionStatus::Testing;
        connection.detail = {};
    }
    sendChangeMessage();

    if (p == nullptr)
    {
        const juce::ScopedLock sl (lock);
        connection.status = ConnectionStatus::Unknown;
        connection.detail = "No provider configured";
        return;
    }

    connectionWorker.clearPending();
    const bool posted = connectionWorker.post ([this, p, token]
    {
        auto result = p->testConnection (*token);
        if (token->isCancelled())
            return;
        {
            const juce::ScopedLock sl (lock);
            if (connectionToken != token)
                return;                          // superseded
            connection = result;
        }
        logging::info ("connection", toDisplayString (result.status) + (result.detail.isNotEmpty() ? " - " + result.detail : juce::String()));
        sendChangeMessage();
    });
    juce::ignoreUnused (posted);
}

// ---------------------------------------------------------------------------

GenerationController::EnqueueResult GenerationController::enqueue (GenerationRequest request)
{
    EnqueueResult r;
    if (shuttingDown.load())
    {
        r.error = ProviderError::make (ErrorCode::Cancelled);
        return r;
    }
    if (deps.isLicensed && ! deps.isLicensed())
    {
        r.error = ProviderError::make (ErrorCode::NotLicensed);
        return r;
    }
    if (auto problem = request.validate(); problem.isNotEmpty())
    {
        r.error = ProviderError::make (ErrorCode::InvalidRequest, problem);
        r.error.userMessage = problem;
        return r;
    }

    auto job = std::make_shared<Job>();
    if (request.requestId.isEmpty())
        request.requestId = juce::Uuid().toDashedString();
    job->snap.localJobId = request.requestId;
    job->snap.request = std::move (request);
    job->snap.state = JobState::Queued;
    job->snap.statusText = "Queued";

    {
        const juce::ScopedLock sl (lock);
        if (provider == nullptr)
        {
            r.error = ProviderError::make (ErrorCode::BridgeNotConfigured);
            return r;
        }
        if ((int) pending.size() >= maxQueued)
        {
            r.error = ProviderError::make (ErrorCode::QueueFull);
            return r;
        }
        pending.push_back (job);
    }

    if (! generationWorker.post ([this, job] { runJob (job); }))
    {
        const juce::ScopedLock sl (lock);
        pending.erase (std::remove (pending.begin(), pending.end(), job), pending.end());
        r.error = ProviderError::make (ErrorCode::QueueFull);
        return r;
    }

    logging::info ("generation", "Queued job " + job->snap.localJobId + " (" + toLabel (job->snap.request.type) + ")");
    sendChangeMessage();
    r.ok = true;
    r.localJobId = job->snap.localJobId;
    return r;
}

void GenerationController::cancelActive()
{
    JobPtr j;
    {
        const juce::ScopedLock sl (lock);
        j = active;
    }
    if (j != nullptr)
    {
        logging::info ("generation", "Cancel requested for " + j->snap.localJobId);
        j->token->cancel();
    }
}

void GenerationController::cancelAll()
{
    std::deque<JobPtr> dropped;
    JobPtr current;
    {
        const juce::ScopedLock sl (lock);
        dropped.swap (pending);
        current = active;
    }
    for (auto& j : dropped)
    {
        j->token->cancel();
        const juce::ScopedLock sl (lock);
        j->snap.state = JobState::Cancelled;
        j->snap.statusText = "Cancelled";
    }
    if (current != nullptr)
        current->token->cancel();
    if (! shuttingDown.load())
        sendChangeMessage();
}

void GenerationController::update (const JobPtr& job, std::function<void (JobSnapshot&)> change)
{
    {
        const juce::ScopedLock sl (lock);
        change (job->snap);
    }
    sendChangeMessage();
}

void GenerationController::finish (const JobPtr& job, JobState state, ProviderError error)
{
    {
        const juce::ScopedLock sl (lock);
        job->snap.state = state;
        job->snap.error = error;
        job->snap.statusText = state == JobState::Complete ? "Done"
                             : state == JobState::Cancelled ? "Cancelled"
                             : error.userMessage;
        if (state == JobState::Complete)
            job->snap.progress = 1.0;
        if (active == job)
            active = nullptr;
        lastFinished = job;
    }

    if (state == JobState::Failed)
    {
        logging::warning ("generation", "Job " + job->snap.localJobId + " failed: " + toId (error.code) + " - " + error.technicalDetail);
        if (auto* l = ContextLogger::getInstance())
            l->recordErrorCode (toId (error.code));
    }
    else
    {
        logging::info ("generation", "Job " + job->snap.localJobId + " " + toString (state));
    }
    sendChangeMessage();
}

void GenerationController::runJob (const JobPtr& job)
{
    std::shared_ptr<IAudioGenerationProvider> p;
    {
        const juce::ScopedLock sl (lock);
        pending.erase (std::remove (pending.begin(), pending.end(), job), pending.end());
        if (job->token->isCancelled() || shuttingDown.load())
        {
            job->snap.state = JobState::Cancelled;
            lastFinished = job;
            return;
        }
        active = job;
        p = provider;
        job->snap.startedMs = juce::Time::getMillisecondCounter();
    }

    if (p == nullptr)
    {
        finish (job, JobState::Failed, ProviderError::make (ErrorCode::BridgeNotConfigured));
        return;
    }

    try
    {
        runPipeline (job, *p);
    }
    catch (const std::exception& e)
    {
        finish (job, JobState::Failed, ProviderError::make (ErrorCode::Internal, juce::String ("Exception: ") + e.what()));
    }
    catch (...)
    {
        finish (job, JobState::Failed, ProviderError::make (ErrorCode::Internal, "Unknown exception"));
    }
}

void GenerationController::runPipeline (const JobPtr& job, IAudioGenerationProvider& p)
{
    auto& token = *job->token;
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutSeconds.load() * 1000u;
    GenerationRequest request;
    {
        const juce::ScopedLock sl (lock);
        request = job->snap.request;
    }

    auto cancelled = [&]
    {
        if (! token.isCancelled())
            return false;
        juce::String remote;
        {
            const juce::ScopedLock sl (lock);
            remote = job->snap.remoteJobId;
        }
        if (remote.isNotEmpty() && ! shuttingDown.load())
        {
            CancellationToken quick;   // fresh token: the job's token is already cancelled
            p.cancel (remote, quick);
        }
        finish (job, JobState::Cancelled, ProviderError::make (ErrorCode::Cancelled));
        return true;
    };

    // ---- 1. Optional audio context (only when the user explicitly enabled it) ----
    // The user asked for their track to be sent, so it is sent or the job stops with the reason.
    // It is never silently dropped (that would spend credits on a result made without it).
    TempFileGuard audioContextGuard;
    if (request.audioContext.has_value())
    {
        // What we know about the bridge may be stale (it was restarted or updated since the last
        // Test Connection), so ask it again before deciding it can't take audio.
        if (! p.getCapabilities().has (Feature::AudioConditioning))
        {
            update (job, [] (JobSnapshot& s) { s.state = JobState::Submitting; s.statusText = "Checking that your bridge takes your track..."; });
            auto fresh = p.testConnection (token);
            if (cancelled()) return;
            if (fresh.capabilities.has_value())
            {
                {
                    const juce::ScopedLock sl (lock);
                    connection = fresh;
                }
                sendChangeMessage();
            }
        }
        const auto modes = p.getCapabilities().usableContextModes();
        if (modes.isEmpty())
        {
            auto e = ProviderError::make (ErrorCode::Unsupported, "Send my track is on, but the bridge does not list audio_conditioning in /context/v1/capabilities");
            e.userMessage = "Your track wasn't sent: this bridge doesn't accept track audio. Start the Kie.ai bridge from the V2.1 folder "
                            "(version 1.1.0 or newer), click Test Connection, then try again.";
            finish (job, JobState::Failed, e);
            return;
        }
        if (! modes.contains (request.contextMode))
            request.contextMode = modes[0];

        std::optional<AudioConditioningPayload> payload;
        if (deps.makeAudioContext)
            payload = deps.makeAudioContext (request.requestId);
        if (! payload.has_value())
        {
            auto e = ProviderError::make (ErrorCode::InvalidRequest, "Send my track is on, but there was no captured or recent track audio to send");
            e.userMessage = "Your track wasn't sent: Context hasn't heard any audio on this track yet. Put Context on the track you want, "
                            "play a few bars (or capture 8 bars), then generate again.";
            finish (job, JobState::Failed, e);
            return;
        }
        request.audioContext = payload;
        audioContextGuard.file = payload->wavFile;
        {
            const juce::ScopedLock sl (lock);
            job->snap.request.contextMode = request.contextMode;
        }
    }

    // ---- 2. Submit (retried only when the bridge was unreachable, i.e. never received it) ----
    juce::String sending = "Sending request...";
    if (request.audioContext.has_value())
        sending = "Sending your " + (request.audioContext->bars > 0 ? juce::String (request.audioContext->bars) + " bars"
                                                                    : juce::String (juce::roundToInt (request.audioContext->seconds)) + " s of your track")
                + " (" + contextmode::label (request.contextMode) + ")...";
    update (job, [sending] (JobSnapshot& s) { s.state = JobState::Submitting; s.statusText = sending; s.progress = -1.0; });

    SubmitResult submitted;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        if (cancelled()) return;
        submitted = p.submit (request, token);
        if (submitted.ok || submitted.error.code != ErrorCode::BridgeOffline)
            break;
        if (token.sleepOrCancelled (1000 << attempt))
            break;
    }
    audioContextGuard.file.deleteFile();
    if (cancelled()) return;
    if (! submitted.ok)
    {
        finish (job, JobState::Failed, submitted.error);
        return;
    }
    update (job, [&] (JobSnapshot& s) { s.remoteJobId = submitted.jobId; s.state = JobState::Generating; s.statusText = "Generating..."; });

    // ---- 3. Poll until finished ----
    PollResult done;
    if (submitted.immediateAudioUrl.isNotEmpty())
    {
        done.ok = true;
        done.state = JobState::Complete;
        done.audioLocation = submitted.immediateAudioUrl;
    }
    else
    {
        int consecutiveErrors = 0;
        const int interval = juce::jlimit (50, 60000, p.pollIntervalMs());
        for (;;)
        {
            if (cancelled()) return;
            if (juce::Time::getMillisecondCounter() > deadline)
            {
                CancellationToken quick;
                p.cancel (submitted.jobId, quick);
                finish (job, JobState::Failed, ProviderError::make (ErrorCode::Timeout,
                        "No result after " + juce::String (timeoutSeconds.load()) + " s"));
                return;
            }

            const int wait = juce::jmin (16000, interval << juce::jmin (consecutiveErrors, 4));
            if (token.sleepOrCancelled (wait)) { cancelled(); return; }

            update (job, [] (JobSnapshot& s) { if (s.state == JobState::Generating || s.state == JobState::Queued) s.state = JobState::Polling; });
            auto polled = p.poll (submitted.jobId, token);
            if (cancelled()) return;

            if (! polled.ok)
            {
                if (polled.error.retryable && ++consecutiveErrors <= 5)
                {
                    update (job, [&] (JobSnapshot& s) { s.state = JobState::Generating; s.statusText = "Waiting for bridge (retry " + juce::String (consecutiveErrors) + ")..."; });
                    logging::warning ("generation", "Poll failed (" + toId (polled.error.code) + "), retrying: " + polled.error.technicalDetail);
                    continue;
                }
                finish (job, JobState::Failed, polled.error);
                return;
            }
            consecutiveErrors = 0;

            if (polled.state == JobState::Failed)    { finish (job, JobState::Failed, polled.error.isError() ? polled.error : ProviderError::make (ErrorCode::GenerationFailed)); return; }
            if (polled.state == JobState::Cancelled) { finish (job, JobState::Cancelled, ProviderError::make (ErrorCode::Cancelled)); return; }
            if (polled.state == JobState::Complete)  { done = polled; break; }

            update (job, [&] (JobSnapshot& s)
            {
                s.state = polled.state == JobState::Queued ? JobState::Queued : JobState::Generating;
                s.progress = polled.progress;
                s.statusText = polled.state == JobState::Queued ? "Waiting in provider queue..." : "Generating...";
            });
        }
    }

    // ---- 4. Download to a temp file ----
    update (job, [] (JobSnapshot& s) { s.state = JobState::Downloading; s.statusText = "Downloading audio..."; s.progress = -1.0; });
    auto tempDir = deps.tempFolder ? deps.tempFolder() : juce::File::getSpecialLocation (juce::File::tempDirectory);
    tempDir.createDirectory();
    TempFileGuard tempGuard { tempDir.getChildFile (filenames::sanitizeComponent (job->snap.localJobId, 64) + ".part") };

    DownloadResult downloaded;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        if (cancelled()) return;
        downloaded = p.download (done, tempGuard.file, token);
        if (downloaded.ok || ! downloaded.error.retryable)
            break;
        if (token.sleepOrCancelled (1000 << attempt))
            break;
    }
    if (cancelled()) return;
    if (! downloaded.ok)
    {
        finish (job, JobState::Failed, downloaded.error);
        return;
    }

    // ---- 5. Validate with a real decoder, then atomically move into the managed folder ----
    update (job, [] (JobSnapshot& s) { s.state = JobState::Processing; s.statusText = "Checking audio..."; });
    const auto info = AudioFileValidator::validate (tempGuard.file);
    if (! info.ok)
    {
        finish (job, JobState::Failed, ProviderError::make (ErrorCode::DecodeFailed, info.error));
        return;
    }
    if (cancelled()) return;

    auto folder = deps.generatedFolder ? deps.generatedFolder() : tempDir;
    if (! folder.createDirectory())
    {
        finish (job, JobState::Failed, ProviderError::make (ErrorCode::StorageFailed, "Cannot create " + folder.getFullPathName()));
        return;
    }

    const auto now = juce::Time::getCurrentTime();
    const auto baseName = filenames::generatedBaseName (deps.productName, request.bpm, request.key,
                                                        request.mode.isNotEmpty() ? (request.mode.equalsIgnoreCase ("minor") ? "Minor" : "Major") : juce::String(),
                                                        toLabel (request.type), now);
    auto finalFile = filenames::uniqueFile (folder, baseName, info.extension);
    if (! tempGuard.file.moveFileTo (finalFile))
    {
        if (! tempGuard.file.copyFileTo (finalFile))
        {
            finish (job, JobState::Failed, ProviderError::make (ErrorCode::StorageFailed, "Cannot write " + finalFile.getFullPathName()));
            return;
        }
    }
    tempGuard.file = juce::File();   // now owned by the managed folder

    GenerationResult result;
    result.generationId = job->snap.localJobId;
    result.providerId = p.id();
    result.remoteJobId = submitted.jobId;
    result.status = JobState::Complete;
    result.prompt = request.userPrompt;
    result.expandedPrompt = request.expandedPrompt;
    result.creationTimestamp = now;
    result.remoteUrl = done.audioLocation.startsWith ("mock://") ? juce::String() : redaction::safeUrlForDisplay (done.audioLocation);
    result.localFile = finalFile;
    result.durationSeconds = info.durationSeconds();
    result.sampleRate = info.sampleRate;
    result.channelCount = info.channels;
    result.type = request.type;
    result.bpm = request.bpm;
    result.key = request.key;
    result.mode = request.mode;
    result.lastUsedMs = now.toMilliseconds();
    if (request.seed) result.metadata.set ("seed", *request.seed);
    if (request.variationOfGenerationId.isNotEmpty()) result.metadata.set ("variationOf", request.variationOfGenerationId);
    result.metadata.set ("type", toId (request.type));
    if (request.audioContext.has_value() && request.contextMode.isNotEmpty())
        result.metadata.set ("contextMode", request.contextMode);

    if (deps.onCompleted)
        deps.onCompleted (result);

    {
        const juce::ScopedLock sl (lock);
        lastResult = result;
    }
    logging::info ("generation", "Saved " + finalFile.getFileName() + " (" + juce::String (info.durationSeconds(), 2) + " s, "
                                 + juce::String ((int) info.sampleRate) + " Hz, " + juce::String (info.channels) + " ch)");
    finish (job, JobState::Complete);
}
} // namespace context
