#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BuildInfo.h"
#include "Core/AppPaths.h"
#include "Generation/LocalGenerationBridgeProvider.h"
#include "Generation/MockAudioGenerationProvider.h"
#include "Utils/Redaction.h"

namespace context
{
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
        return analysisController->writeAudioContext (shared->tempFolder().getChildFile (requestId + "-context.wav"), 8.0);
    };
    generationController = std::make_unique<GenerationController> (deps);

    const auto s = shared->settings().get();
    generationController->setGenerationTimeoutSeconds (s.generationTimeoutSeconds);
    previewPlayer.setGainDb (s.previewGainDb);
    rebuildProvider (true);

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
    if (auto* playHead = getPlayHead())
    {
        if (const auto pos = playHead->getPosition())
        {
            const auto bpm = pos->getBpm();
            hostBpm.store (bpm.hasValue() && *bpm > 0.0 ? *bpm : 0.0, std::memory_order_relaxed);
            hostPlaying.store (pos->getIsPlaying(), std::memory_order_relaxed);
            if (const auto ppq = pos->getPpqPosition())
                hostPpq.store (*ppq, std::memory_order_relaxed);
            if (const auto ts = pos->getTimeSignature())
            {
                hostTsNum.store (ts->numerator, std::memory_order_relaxed);
                hostTsDen.store (ts->denominator, std::memory_order_relaxed);
            }
        }
    }

    // Capture the incoming (pre-preview) audio: lock-free try, never waits.
    if (numIn > 0)
        captureBuffer.push (buffer.getArrayOfReadPointers(), juce::jmin (numIn, buffer.getNumChannels()), numSamples);

    // Audition the generated sound (only while the user pressed Play; never in offline renders).
    previewPlayer.renderAdd (buffer, numSamples, isNonRealtime());
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
    tree.setProperty ("tab", s.tab, nullptr);
    tree.setProperty ("selectedResultId", s.selectedResultId, nullptr);
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
    s.tab = tree.getProperty ("tab", "generate").toString().substring (0, 16);
    s.selectedResultId = tree.getProperty ("selectedResultId").toString().substring (0, 64);
    setUiState (s);
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
        shared->applyLogLevel();
        if (s.captureSeconds != capturedSecondsSetting && currentSampleRate.load() > 0)
        {
            capturedSecondsSetting = s.captureSeconds;
            captureBuffer.prepare (currentSampleRate.load(), s.captureSeconds);
        }
    }
    else if (source == generationController.get())
    {
        // When a job completes, make its result the selected one (also when the editor is closed).
        if (const auto last = generationController->getLastResult())
        {
            const juce::ScopedLock sl (uiLock);
            if (ui.selectedResultId != last->generationId && generationController->getCurrentJob().state == JobState::Complete
                && generationController->getCurrentJob().localJobId == last->generationId)
                ui.selectedResultId = last->generationId;
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

    const bool consent = shared->settings().get().allowSendAudioContext;
    if (s.sendAudioContext && consent && generationController->getCapabilities().has (Feature::AudioConditioning))
        r.audioContext = AudioConditioningPayload{};   // filled on the worker at send time
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
    return r;
}

GenerationController::EnqueueResult ContextAudioProcessor::generate()
{
    return generationController->enqueue (makeRequest (getUiState()));
}

GenerationController::EnqueueResult ContextAudioProcessor::regenerate (const GenerationResult& source)
{
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
