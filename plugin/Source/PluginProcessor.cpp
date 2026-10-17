#include "PluginProcessor.h"
#include "Audio/Resampler.h"
#include "PluginEditor.h"
#include "BuildInfo.h"
#include "Core/AppPaths.h"
#include "Generation/LocalGenerationBridgeProvider.h"
#include "Generation/MockAudioGenerationProvider.h"
#include "Utils/Redaction.h"

namespace context
{
namespace
{
    /** The captured context as a 44.1 kHz stereo 16-bit WAV (what generation services expect). */
    std::optional<AudioConditioningPayload> writeContextForUpload (const AudioAsset& asset, int bars, const juce::File& file)
    {
        constexpr double rate = 44100.0;
        constexpr double maxSeconds = 90.0;
        auto stereo = assets::toStereo (asset.audio);
        if (std::abs (asset.sampleRate - rate) > 0.5)
            stereo = dsp::resampler::convertRate (stereo, asset.sampleRate, rate);
        if (stereo.getNumSamples() > (int) (maxSeconds * rate))
            stereo.setSize (2, (int) (maxSeconds * rate), true, true, true);
        file.getParentDirectory().createDirectory();
        juce::String err;
        if (stereo.getNumSamples() == 0 || ! assets::writeWav (stereo, rate, file, 16, err))
            return std::nullopt;
        AudioConditioningPayload p;
        p.wavFile = file;
        p.sampleRate = rate;
        p.seconds = stereo.getNumSamples() / rate;
        p.bars = bars;
        return p;
    }
}

ContextAudioProcessor::ContextAudioProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    previewWorker = std::make_unique<SerialWorker> ("Context preview", 4);

    analysisController = std::make_unique<AnalysisController> (captureBuffer, [this]
    {
        const double b = hostBpm.load();
        return b > 0.0 ? std::optional<double> (b) : std::nullopt;
    });

    GenerationController::Dependencies deps;
    deps.productName = build::productName;
    deps.isLicensed = [this] { return isLicensed(); };
    deps.generatedFolder = [this] { return shared->generatedFolder(); };
    deps.tempFolder = [this] { return shared->tempFolder(); };
    deps.onCompleted = [this] (const GenerationResult& r)
    {
        shared->history().add (r);
        shared->scheduleCacheMaintenance();
    };
    deps.makeAudioContext = [this] (const juce::String& requestId) -> std::optional<AudioConditioningPayload>
    {
        // Double-check consent at the moment of sending (settings may have changed).
        if (! shared->settings().get().allowSendAudioContext)
            return std::nullopt;
        const auto file = shared->tempFolder().getChildFile (requestId + "-context.wav");
        // Prefer the captured context: exactly N bars, full quality, so the bridge can make
        // something that lines up with it. Fall back to the last seconds of playback.
        if (sessionController != nullptr)
            if (auto ctx = sessionController->contextForUpload(); ctx.asset != nullptr)
                if (auto p = writeContextForUpload (*ctx.asset, ctx.bars, file))
                    return p;
        return analysisController->writeAudioContext (file, 8.0);
    };
    generationController = std::make_unique<GenerationController> (deps);

    const auto s = shared->settings().get();
    generationController->setGenerationTimeoutSeconds (s.generationTimeoutSeconds);
    previewPlayer.setGainDb (s.previewGainDb);
    rebuildProvider (true);

    // V2 workspace: context capture, fit, effects, two-lane preview, processed export.
    SessionController::Dependencies sd;
    sd.recorder = &contextRecorder;
    sd.engine = &previewEngine;
    sd.chain = &effectChain;
    sd.analysis = analysisController.get();
    sd.host = [this]
    {
        SessionController::HostSnapshot h;
        h.sampleRate = currentSampleRate.load();
        const double b = hostBpm.load();
        if (b > 0.0) h.bpm = b;
        h.signature = { hostTsNum.load(), hostTsDen.load() };
        h.playing = hostPlaying.load();
        return h;
    };
    sd.findGenerated = [this] (const juce::String& id) { return shared->history().find (id); };
    sd.contextCacheFolder = [] { return AppPaths::contextCacheDir(); };
    sd.exportFolder = [this] { return shared->generatedFolder().getChildFile ("Processed"); };
    sd.stagingFolder = [this] { return shared->tempFolder(); };
    sd.productName = build::productName;
    sessionController = std::make_unique<SessionController> (sd);
    effectChain.onChanged = [this] { if (sessionController != nullptr) sessionController->processingChanged(); };
    previewEngine.setMasterGainDb (s.previewGainDb + 6.0f);   // lanes at unity; Settings' preview level applies (default -6 dB -> 0 dB offset)

    shared->settings().addChangeListener (this);
    generationController->addChangeListener (this);
    startTimer (1000);
}

ContextAudioProcessor::~ContextAudioProcessor()
{
    alive = false;
    stopTimer();
    shared->settings().removeChangeListener (this);
    generationController->removeChangeListener (this);

    // Order matters: stop workers (cancelling in-flight HTTP) before the things they use go away.
    effectChain.onChanged = nullptr;
    sessionController.reset();
    generationController.reset();
    analysisController.reset();
    previewWorker.reset();
}

const juce::String ContextAudioProcessor::getName() const { return build::productName; }

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

void ContextAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize = samplesPerBlock;
    capturedSecondsSetting = shared->settings().get().captureSeconds;
    captureBuffer.prepare (sampleRate, capturedSecondsSetting);
    previewPlayer.setHostSampleRate (sampleRate);
    // The audio thread is not running here: safe to (re)allocate realtime state.
    contextRecorder.resetNonRealtime();
    previewEngine.prepare (sampleRate, samplesPerBlock);
    effectChain.prepare (sampleRate, samplesPerBlock);
    // Plugin latency stays 0: effect latency is compensated inside the preview engine,
    // and the DAW signal passes through untouched.
    setLatencySamples (0);
}

void ContextAudioProcessor::releaseResources() {}

bool ContextAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void ContextAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels();

    for (int ch = numIn; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    // Host transport info (never assume BPM exists).
    ContextRecorder::HostPosition hostPos;
    if (auto* playHead = getPlayHead())
    {
        if (const auto pos = playHead->getPosition())
        {
            const auto bpm = pos->getBpm();
            hostBpm.store (bpm.hasValue() && *bpm > 0.0 ? *bpm : 0.0, std::memory_order_relaxed);
            hostPlaying.store (pos->getIsPlaying(), std::memory_order_relaxed);
            hostPlayingKnown.store (true, std::memory_order_relaxed);
            hostPos.playing = pos->getIsPlaying();
            hostPos.playingKnown = true;
            if (bpm.hasValue() && *bpm > 0.0)
                hostPos.bpm = *bpm;
            if (const auto ppq = pos->getPpqPosition())
            {
                hostPpq.store (*ppq, std::memory_order_relaxed);
                hostPos.ppq = *ppq;
            }
            if (const auto lastBar = pos->getPpqPositionOfLastBarStart())
                hostPos.lastBarStartPpq = *lastBar;
            if (const auto ts = pos->getTimeSignature())
            {
                hostTsNum.store (ts->numerator, std::memory_order_relaxed);
                hostTsDen.store (ts->denominator, std::memory_order_relaxed);
                hostPos.signature = { ts->numerator, ts->denominator };
            }
        }
    }

    // Capture the incoming (pre-preview) audio: lock-free try, never waits.
    if (numIn > 0)
    {
        const int chans = juce::jmin (numIn, buffer.getNumChannels());
        captureBuffer.push (buffer.getArrayOfReadPointers(), chans, numSamples);
        contextRecorder.process (buffer.getArrayOfReadPointers(), chans, numSamples, hostPos);
    }

    // Audition (only while the user pressed Play; never in offline renders):
    // the single-file preview, and the two-lane workspace with its effect rack.
    previewPlayer.renderAdd (buffer, numSamples, isNonRealtime());
    previewEngine.renderAdd (buffer, numSamples, isNonRealtime(), effectChain);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

void ContextAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    UiState s = getUiState();
    juce::ValueTree tree ("ContextState");
    tree.setProperty ("version", 1, nullptr);
    tree.setProperty ("prompt", s.prompt.substring (0, 1000), nullptr);
    tree.setProperty ("type", toId (s.type), nullptr);
    tree.setProperty ("useTrackContext", s.useTrackContext, nullptr);
    tree.setProperty ("sendAudioContext", s.sendAudioContext, nullptr);
    tree.setProperty ("contextMode", s.contextMode, nullptr);
    tree.setProperty ("tab", s.tab, nullptr);
    tree.setProperty ("selectedResultId", s.selectedResultId, nullptr);
    // V2 workspace: settings + references only (captured/generated audio stays in files).
    if (sessionController != nullptr)
    {
        tree.setProperty ("version", 2, nullptr);
        tree.setProperty ("session", juce::JSON::toString (sessionController->getState().toVar(), true), nullptr);
        tree.setProperty ("chain", juce::JSON::toString (effectChain.toVar(), true), nullptr);
    }
    // Deliberately NOT stored here: credentials, license data, audio, history.
    if (auto xml = tree.createXml())
        copyXmlToBinary (*xml, destData);
}

void ContextAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0 || sizeInBytes > 1024 * 1024)
        return;
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr)
        return;
    auto tree = juce::ValueTree::fromXml (*xml);
    if (! tree.hasType ("ContextState"))
        return;

    UiState s;
    s.prompt = tree.getProperty ("prompt").toString().substring (0, 1000);
    s.type = generationTypeFromId (tree.getProperty ("type").toString()).value_or (GenerationType::OneShot);
    s.useTrackContext = (bool) tree.getProperty ("useTrackContext", true);
    s.sendAudioContext = (bool) tree.getProperty ("sendAudioContext", false);
    {
        const auto cm = tree.getProperty ("contextMode", "layer").toString();
        s.contextMode = (cm == contextmode::cover || cm == contextmode::extend) ? cm : contextmode::layer;
    }
    s.tab = tree.getProperty ("tab", "generate").toString().substring (0, 16);
    s.selectedResultId = tree.getProperty ("selectedResultId").toString().substring (0, 64);
    setUiState (s);

    // V2 fields are optional: projects saved by older versions simply don't have them.
    const auto sessionJson = tree.getProperty ("session").toString();
    const auto chainJson = tree.getProperty ("chain").toString();
    if (sessionJson.isNotEmpty() || chainJson.isNotEmpty())
        restoreSession (juce::JSON::parse (sessionJson), juce::JSON::parse (chainJson));
}

void ContextAudioProcessor::restoreSession (const juce::var& sessionVar, const juce::var& chainVar)
{
    auto apply = [sessionVar, chainVar] (ContextAudioProcessor& p)
    {
        if (p.sessionController != nullptr)
            p.sessionController->restore (SessionState::fromVar (sessionVar), chainVar);
    };
    // Hosts may restore state from any thread; the session lives on the message thread.
    if (juce::MessageManager::existsAndIsCurrentThread())
        apply (*this);
    else
        juce::MessageManager::callAsync ([weak = juce::WeakReference<ContextAudioProcessor> (this), apply]
        {
            if (auto* p = weak.get())
                apply (*p);
        });
}

ContextAudioProcessor::UiState ContextAudioProcessor::getUiState() const
{
    const juce::ScopedLock sl (uiLock);
    return ui;
}

void ContextAudioProcessor::setUiState (const UiState& s)
{
    {
        const juce::ScopedLock sl (uiLock);
        ui = s;
    }
    stateChanged.sendChangeMessage();
}

ContextAudioProcessor::HostInfo ContextAudioProcessor::getHostInfo() const
{
    HostInfo h;
    const double b = hostBpm.load();
    if (b > 0.0) h.bpm = b;
    h.playing = hostPlaying.load();
    h.ppq = hostPpq.load();
    h.tsNum = hostTsNum.load();
    h.tsDen = hostTsDen.load();
    h.sampleRate = currentSampleRate.load();
    return h;
}

bool ContextAudioProcessor::isLicensed() const
{
    return shared->license().isLicensed();
}

// ---------------------------------------------------------------------------
// Providers / settings
// ---------------------------------------------------------------------------

juce::String ContextAudioProcessor::providerKey (const Settings& s) const
{
    return s.providerId + "|" + s.bridgeUrl + "|" + s.bridgeProfileId + "|" + juce::String ((int) s.allowNonLocalBridge) + "|"
         + s.acknowledgedNonLocalHost + "|" + juce::String (s.mockLatencyScale) + "|" + juce::String (s.mockFailurePercent) + "|"
         + juce::String ((int) s.developerMode) + "|" + juce::String (AppPaths::bridgeProfilesFile().getLastModificationTime().toMilliseconds());
}

void ContextAudioProcessor::rebuildProvider (bool force)
{
    const auto s = shared->settings().get();
    const auto key = providerKey (s);
    if (! force && key == currentProviderKey)
        return;
    currentProviderKey = key;

    std::shared_ptr<IAudioGenerationProvider> provider;
    if (s.providerId == "mock")
    {
        MockAudioGenerationProvider::Options o;
        o.latencyScale = s.developerMode ? s.mockLatencyScale : 1.0;
        o.failurePercent = s.developerMode ? s.mockFailurePercent : 0;
        provider = std::make_shared<MockAudioGenerationProvider> (o);
    }
    else
    {
        const auto profiles = BridgeProfileSet::load (AppPaths::bridgeProfilesFile());
        for (auto& p : profiles.problems)
            logging::warning ("bridge", "bridge_profiles.json: " + p);
        const auto* profile = profiles.find (s.bridgeProfileId);
        if (profile == nullptr)
        {
            logging::warning ("bridge", "Bridge profile '" + s.bridgeProfileId + "' not found; using " + profiles.profiles.front().id);
            profile = &profiles.profiles.front();
        }
        LocalGenerationBridgeProvider::Options o;
        o.baseUrl = s.bridgeUrl;
        o.profile = *profile;
        o.allowNonLocal = s.allowNonLocalBridge;
        o.acknowledgedNonLocalHost = s.acknowledgedNonLocalHost;
        provider = std::make_shared<LocalGenerationBridgeProvider> (o);
    }
    logging::info ("provider", "Using " + provider->displayName()
                               + (s.providerId == "bridge" ? " at " + redaction::safeUrlForDisplay (s.bridgeUrl) + " (profile " + s.bridgeProfileId + ")" : juce::String()));
    generationController->setProvider (provider);
}

void ContextAudioProcessor::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (! alive.load())
        return;

    if (source == &shared->settings())
    {
        const auto s = shared->settings().get();
        rebuildProvider();
        generationController->setGenerationTimeoutSeconds (s.generationTimeoutSeconds);
        previewPlayer.setGainDb (s.previewGainDb);
        previewEngine.setMasterGainDb (s.previewGainDb + 6.0f);
        shared->applyLogLevel();
        if (s.captureSeconds != capturedSecondsSetting && currentSampleRate.load() > 0)
        {
            capturedSecondsSetting = s.captureSeconds;
            captureBuffer.prepare (currentSampleRate.load(), s.captureSeconds);
        }
    }
    else if (source == generationController.get())
    {
        // When a job completes, make its result the selected one (also when the editor is closed)
        // and load it into the workspace's output lane.
        if (const auto last = generationController->getLastResult())
        {
            bool fresh = false;
            {
                const juce::ScopedLock sl (uiLock);
                if (generationController->getCurrentJob().state == JobState::Complete
                    && generationController->getCurrentJob().localJobId == last->generationId)
                {
                    ui.selectedResultId = last->generationId;
                    fresh = lastAutoLoadedResult != last->generationId;
                    lastAutoLoadedResult = last->generationId;
                }
            }
            if (fresh && sessionController != nullptr)
                sessionController->generationArrived (*last);
        }
        // A fill whose generation failed or was cancelled: forget the selection it was for.
        const auto job = generationController->getCurrentJob();
        if (pendingFillJob.isNotEmpty() && job.localJobId == pendingFillJob
            && (job.state == JobState::Failed || job.state == JobState::Cancelled))
        {
            pendingFillJob = {};
            if (sessionController != nullptr)
                sessionController->cancelPendingFill();
        }
        stateChanged.sendChangeMessage();
    }
}

void ContextAudioProcessor::timerCallback()
{
    previewPlayer.collectGarbage();
    ++timerTicks;
    // Licensing refresh: first check 10 s after load (not during a quick plugin scan), then every 30 min.
    if (timerTicks == 10 || timerTicks % 1800 == 0)
        shared->license().refreshIfDue();
}

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

PromptBuilder::Output ContextAudioProcessor::previewPrompt (const UiState& s) const
{
    const auto settings = shared->settings().get();
    PromptBuilder::Inputs in;
    in.userPrompt = s.prompt;
    in.type = s.type;
    in.expansionEnabled = settings.promptExpansion;
    if (s.useTrackContext)
    {
        in.analysis = analysisController->getLatest();
        const double b = hostBpm.load();
        if (b > 0.0) in.hostBpm = b;
    }
    return PromptBuilder::build (in);
}

GenerationRequest ContextAudioProcessor::makeRequest (const UiState& s) const
{
    const auto built = previewPrompt (s);
    GenerationRequest r;
    r.userPrompt = s.prompt.trim();
    r.expandedPrompt = built.expandedPrompt;
    r.type = s.type;
    r.bpm = built.bpm;
    r.key = built.key;
    r.mode = built.mode;
    r.keyConfidence = built.keyConfidence;
    r.seed = (juce::int64) juce::Random::getSystemRandom().nextInt (0x7fffffff);
    if (s.useTrackContext)
        r.contextAnalysis = analysisController->getLatest();

    // Ask for the selected bar length (one-shots and FX stay short; the fit renderer
    // places them on bar 1). Providers that can't honour durations still get fitted.
    if (sessionController != nullptr && s.type != GenerationType::OneShot && s.type != GenerationType::FX)
    {
        const auto tempo = sessionController->sessionTempo();
        const double seconds = musical::secondsForBars (sessionController->getState().lengthBars, tempo);
        const int maxSeconds = generationController->getCapabilities().maxDurationSeconds;
        if (seconds > 0.0)
            r.durationSeconds = juce::jmin (seconds, (double) juce::jmax (1, maxSeconds));
        if (! r.bpm.has_value())
            r.bpm = tempo.bpm;
    }

    // With "Send my track" on, the request always asks for it; the worker re-checks the bridge
    // (capabilities here may be stale) and either sends the track or stops with the reason.
    const bool consent = shared->settings().get().allowSendAudioContext;
    const auto modes = generationController->getCapabilities().usableContextModes();
    const auto provider = generationController->getProvider();
    const bool bridge = provider != nullptr && provider->id() == "bridge";
    if (s.sendAudioContext && consent && (bridge || ! modes.isEmpty()))
    {
        r.audioContext = AudioConditioningPayload{};   // filled on the worker at send time
        r.contextMode = modes.isEmpty() || modes.contains (s.contextMode) ? s.contextMode : modes[0];
        if (r.contextMode.isEmpty())
            r.contextMode = contextmode::layer;
    }
    return r;
}

GenerationRequest ContextAudioProcessor::requestFromResult (const GenerationResult& src) const
{
    GenerationRequest r;
    r.userPrompt = src.prompt;
    r.expandedPrompt = src.expandedPrompt.isNotEmpty() ? src.expandedPrompt : src.prompt;
    r.type = src.type;
    r.bpm = src.bpm;
    r.key = src.key;
    r.mode = src.mode;
    r.seed = (juce::int64) juce::Random::getSystemRandom().nextInt (0x7fffffff);
    if (sessionController != nullptr && src.type != GenerationType::OneShot && src.type != GenerationType::FX)
    {
        const double seconds = musical::secondsForBars (sessionController->getState().lengthBars, sessionController->sessionTempo());
        if (seconds > 0.0)
            r.durationSeconds = juce::jmin (seconds, (double) juce::jmax (1, generationController->getCapabilities().maxDurationSeconds));
    }
    // Same style: a result that was generated on the track context is regenerated on it again.
    const auto mode = src.metadata.getWithDefault ("contextMode", juce::var()).toString();
    const auto modes = generationController->getCapabilities().usableContextModes();
    const auto provider = generationController->getProvider();
    const bool bridge = provider != nullptr && provider->id() == "bridge";
    if (mode.isNotEmpty() && (modes.contains (mode) || (bridge && modes.isEmpty())) && shared->settings().get().allowSendAudioContext)
    {
        r.audioContext = AudioConditioningPayload{};
        r.contextMode = mode;
    }
    return r;
}

GenerationController::EnqueueResult ContextAudioProcessor::generate()
{
    // A plain Generate replaces the lane, so a pending "fill selection" is dropped.
    pendingFillJob = {};
    if (sessionController != nullptr)
        sessionController->cancelPendingFill();
    return generationController->enqueue (makeRequest (getUiState()));
}

GenerationController::EnqueueResult ContextAudioProcessor::regenerate (const GenerationResult& source)
{
    pendingFillJob = {};
    if (sessionController != nullptr)
        sessionController->cancelPendingFill();
    return generationController->enqueue (requestFromResult (source));
}

bool ContextAudioProcessor::canVariation() const
{
    return generationController->getCapabilities().has (Feature::Variation);
}

GenerationController::EnqueueResult ContextAudioProcessor::variation (const GenerationResult& source)
{
    if (! canVariation())
    {
        GenerationController::EnqueueResult r;
        r.error = ProviderError::make (ErrorCode::Unsupported, "Provider has no variation capability");
        r.error.userMessage = "This provider doesn't support variations. Use Regenerate instead.";
        return r;
    }
    auto req = requestFromResult (source);
    req.variationOfGenerationId = source.generationId;
    req.variationOfRemoteId = source.remoteJobId;
    if (source.metadata.contains ("seed"))
        req.seed = (juce::int64) source.metadata["seed"] + 1 + juce::Random::getSystemRandom().nextInt (1000);
    return generationController->enqueue (req);
}

GenerationController::EnqueueResult ContextAudioProcessor::fillWithNewTake (double fromBeat, double toBeat)
{
    // Same style as what's on the lane: regenerate the base take's request when it was generated,
    // otherwise use the prompt as it is now.
    GenerationRequest req;
    const auto s = sessionController->getState();
    if (auto r = s.generatedId.isNotEmpty() ? shared->history().find (s.generatedId) : std::nullopt)
        req = requestFromResult (*r);
    else
        req = makeRequest (getUiState());
    auto res = generationController->enqueue (req);
    if (res.ok)
    {
        pendingFillJob = res.localJobId;
        sessionController->requestFill (fromBeat, toBeat);
    }
    return res;
}

void ContextAudioProcessor::analyzeTrack()
{
    analysisController->requestAnalysis();
}

// ---------------------------------------------------------------------------
// Results / preview
// ---------------------------------------------------------------------------

std::optional<GenerationResult> ContextAudioProcessor::getSelectedResult() const
{
    juce::String id;
    {
        const juce::ScopedLock sl (uiLock);
        id = ui.selectedResultId;
    }
    if (id.isEmpty())
        return std::nullopt;
    return shared->history().find (id);
}

void ContextAudioProcessor::selectResult (const juce::String& id)
{
    {
        const juce::ScopedLock sl (uiLock);
        ui.selectedResultId = id;
    }
    if (sessionController != nullptr)
        if (auto r = shared->history().find (id))
            sessionController->loadGenerated (*r);
    stateChanged.sendChangeMessage();
}

juce::String ContextAudioProcessor::getPreviewError() const
{
    const juce::ScopedLock sl (uiLock);
    return previewError;
}

void ContextAudioProcessor::previewResult (const GenerationResult& r)
{
    shared->history().touch (r.generationId);
    if (sessionController != nullptr)
        sessionController->stop();   // one audition at a time
    if (previewPlayer.loadedFile() == r.localFile && previewPlayer.lengthSeconds() > 0)
    {
        previewPlayer.play();
        stateChanged.sendChangeMessage();
        return;
    }

    const auto file = r.localFile;
    previewWorker->clearPending();
    previewWorker->post ([this, file]
    {
        juce::String error;
        const bool ok = previewPlayer.load (file, error);
        {
            const juce::ScopedLock sl (uiLock);
            previewError = ok ? juce::String() : error;
        }
        if (ok)
            previewPlayer.play();
        else
            logging::warning ("preview", "Could not load " + file.getFileName() + ": " + error);
        stateChanged.sendChangeMessage();
    });
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

juce::String ContextAudioProcessor::diagnosticsText() const
{
    const auto s = shared->settings().get();
    const auto conn = generationController->getConnectionResult();
    auto& log = shared->logger();
    juce::String d;
    d << build::productName << " " << build::version << " (build " << build::gitCommit << ", " << build::buildConfig << ")\n"
      << "JUCE " << juce::SystemStats::getJUCEVersion() << "\n"
      << "OS: " << juce::SystemStats::getOperatingSystemName() << (juce::SystemStats::isOperatingSystem64Bit() ? " 64-bit" : "") << "\n"
      << "Host: " << juce::PluginHostType().getHostDescription() << " (" << juce::AudioProcessor::getWrapperTypeDescription (wrapperType) << ")\n"
      << "Sample rate: " << currentSampleRate.load() << " Hz, block size: " << currentBlockSize.load() << "\n"
      << "Channels in/out: " << getTotalNumInputChannels() << "/" << getTotalNumOutputChannels() << "\n"
      << "Provider: " << s.providerId << (s.providerId == "bridge" ? " @ " + redaction::safeUrlForDisplay (s.bridgeUrl) + " (profile " + s.bridgeProfileId + ")" : juce::String()) << "\n"
      << "Connection: " << toDisplayString (conn.status) << (conn.detail.isNotEmpty() ? " - " + conn.detail : juce::String()) << "\n"
      << "License: " << LicenseManager::toString (shared->license().getStatus()) << " " << shared->license().getMaskedKey() << "\n"
      << "Captured audio: " << juce::String (captureBuffer.capturedSeconds(), 1) << " s of " << juce::String (captureBuffer.capacitySeconds(), 0) << " s\n"
      << "History entries: " << shared->history().size() << "\n"
      << "Workspace: tempo " << (sessionController != nullptr ? juce::String (sessionController->sessionTempo().bpm, 2) : juce::String ("-")) << " BPM, "
      << (sessionController != nullptr && sessionController->hasContext() ? "context captured" : "no context") << ", "
      << effectChain.size() << " effect(s), chain latency " << effectChain.latencySamples() << " samples\n"
      << "Recent error codes: " << log.recentErrorCodes().joinIntoString (", ") << "\n"
      << "Log file: " << log.currentLogFile().getFullPathName() << "\n";
    // Log lines are redacted when written; the header is redacted here.
    return redaction::redact (d) + "\nRecent log lines:\n" + log.recentLines (25).joinIntoString ("\n") + "\n";
}

juce::AudioProcessorEditor* ContextAudioProcessor::createEditor()
{
    return new ContextAudioProcessorEditor (*this);
}
} // namespace context
