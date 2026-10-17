#include "SessionController.h"
#include "Analysis/TrackAnalyzer.h"
#include "Audio/Resampler.h"
#include "Storage/Logger.h"
#include "Utils/FilenameSanitizer.h"

namespace context
{
namespace
{
    constexpr int timerHz = 30;
    constexpr int keepCaptures = 30;
    constexpr juce::int64 captureCacheBytes = (juce::int64) 1536 * 1024 * 1024;

    std::shared_ptr<const juce::AudioBuffer<float>> playbackFor (const AudioAsset::Ptr& asset, double rate)
    {
        if (asset == nullptr)
            return nullptr;
        if (std::abs (asset->sampleRate - rate) < 0.5 && asset->numChannels() == 2)
            return std::shared_ptr<const juce::AudioBuffer<float>> (asset, &asset->audio);   // shares the asset's lifetime
        auto stereo = assets::toStereo (asset->audio);
        if (std::abs (asset->sampleRate - rate) >= 0.5)
            stereo = dsp::resampler::convertRate (stereo, asset->sampleRate, rate);
        return std::make_shared<const juce::AudioBuffer<float>> (std::move (stereo));
    }

    juce::String bpmText (double bpm)
    {
        return juce::String (bpm, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    }
}

SessionController::SessionController (Dependencies d) : deps (std::move (d))
{
    jassert (deps.recorder && deps.engine && deps.chain && deps.analysis && deps.host);
    for (auto* w : { &loader, &renderer, &exporter })
        w->onTaskException = [] (const juce::String& what) { logging::error ("session", "Worker task threw: " + what); };
    lastRate = renderRate();
    applyEngineSettings();
    startTimerHz (timerHz);
}

SessionController::~SessionController()
{
    stopTimer();
    if (renderToken)
        renderToken->cancel();
    loader.clearPending();
    renderer.clearPending();
    exporter.clearPending();
    life.reset();   // late callAsync results are dropped from here on
}

template <typename Fn>
void SessionController::onMessageThread (Fn&& fn)
{
    std::weak_ptr<int> token = life;
    juce::MessageManager::callAsync ([token, f = std::forward<Fn> (fn)]() mutable
    {
        if (token.lock() != nullptr)
            f();
    });
}

template <typename Fn>
void SessionController::mutate (Fn&& change)
{
    {
        const juce::ScopedLock sl (stateLock);
        change (state);
    }
    sendChangeMessage();
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

SessionState SessionController::getState() const
{
    const juce::ScopedLock sl (stateLock);
    return state;
}

void SessionController::restore (const SessionState& s, const juce::var& chainState)
{
    {
        const juce::ScopedLock sl (stateLock);
        state = s;
    }
    if (chainState.isObject())
        for (auto& w : deps.chain->fromVar (chainState))
            logging::warning ("session", w);
    applyEngineSettings();

    // Context: reload the cached capture (or report that it's gone).
    contextAsset.reset();
    contextPlayback.reset();
    contextPeaks.reset();
    contextMissing = false;
    if (s.context)
        loadContextFromCache (*s.context);

    // Output source.
    source.reset();
    fitted.reset();
    fittedPeaks.reset();
    hasReport = false;
    outputMissing = false;
    outputMessage = {};
    if (s.sourceKind == SessionState::SourceKind::Generated)
    {
        if (auto r = deps.findGenerated ? deps.findGenerated (s.generatedId) : std::nullopt; r && r->localFile.existsAsFile())
            loadGenerated (*r);
        else
        {
            outputMissing = true;
            outputMessage = "The generated sound used in this project is no longer in Context's library.";
        }
    }
    else if (s.sourceKind == SessionState::SourceKind::Imported)
    {
        const juce::File f (s.importedPath);
        if (f.existsAsFile())
            importFile (f);
        else
        {
            outputMissing = true;
            outputMessage = "Sample not found: " + f.getFileName();
        }
    }
    publishProgram();
    sendChangeMessage();
}

void SessionController::applyEngineSettings()
{
    const auto s = getState();
    auto& e = *deps.engine;
    e.setLaneGainDb (PreviewEngine::contextLane, s.contextGainDb);
    e.setLaneGainDb (PreviewEngine::outputLane, s.outputGainDb);
    e.setLaneMute (PreviewEngine::contextLane, s.contextMute);
    e.setLaneMute (PreviewEngine::outputLane, s.outputMute);
    e.setLaneSolo (PreviewEngine::contextLane, s.contextSolo);
    e.setLaneSolo (PreviewEngine::outputLane, s.outputSolo);
    e.setMode (s.playMode);
    e.setLooping (s.loop);
    e.setCompareOriginal (s.compareOriginal);
    e.setTempo (sessionTempo().bpm);
}

// ---------------------------------------------------------------------------
// Tempo
// ---------------------------------------------------------------------------

std::optional<double> SessionController::hostBpm() const
{
    const auto h = deps.host();
    if (h.bpm && musical::isValidBpm (*h.bpm))
        return h.bpm;
    return std::nullopt;
}

SessionController::TempoSource SessionController::tempoSource() const
{
    if (getState().context)
        return TempoSource::Context;
    return hostBpm() ? TempoSource::Host : TempoSource::Manual;
}

musical::Tempo SessionController::sessionTempo() const
{
    const auto s = getState();
    if (s.context && s.context->tempo.isValid())
        return s.context->tempo;
    const auto h = deps.host();
    if (h.bpm && musical::isValidBpm (*h.bpm))
        return { *h.bpm, h.signature.isValid() ? h.signature : musical::TimeSignature{} };
    return { s.manualBpm, s.manualSignature };
}

bool SessionController::hostTempoDiffers() const
{
    const auto s = getState();
    const auto h = hostBpm();
    return s.context && h && std::abs (*h - s.context->tempo.bpm) > 0.05;
}

void SessionController::setManualTempo (double bpm)
{
    if (! musical::isValidBpm (bpm))
        return;
    const bool affects = tempoSource() == TempoSource::Manual;
    mutate ([bpm] (SessionState& s) { s.manualBpm = bpm; });
    if (affects)
    {
        deps.engine->setTempo (bpm);
        requestRender();
    }
}

double SessionController::renderRate() const
{
    const auto h = deps.host();
    return h.sampleRate > 0.0 ? h.sampleRate : 48000.0;
}

void SessionController::hostConfigurationChanged()
{
    // Polled from the timer as well; this just makes it immediate when the processor knows.
    timerCallback();
}

// ---------------------------------------------------------------------------
// Context capture
// ---------------------------------------------------------------------------

void SessionController::startCapture()
{
    const auto st = deps.recorder->getState();
    if (st == ContextRecorder::State::Armed || st == ContextRecorder::State::Recording)
        return;
    deps.engine->stop();   // don't audition while the user plays the DAW
    const auto h = deps.host();
    const auto s = getState();
    musical::Tempo fallback { s.manualBpm, s.manualSignature };
    if (h.bpm && musical::isValidBpm (*h.bpm))
        fallback = { *h.bpm, h.signature.isValid() ? h.signature : musical::TimeSignature{} };
    if (! deps.recorder->arm (fallback, renderRate(), musical::contextBars))
    {
        notify ("Still finishing the previous capture. Try again in a moment.", true);
        return;
    }
    logging::info ("session", "Capture armed (" + bpmText (fallback.bpm) + " BPM expected, " + juce::String (renderRate(), 0) + " Hz)");
    sendChangeMessage();
}

void SessionController::cancelCapture()
{
    deps.recorder->cancel();
    sendChangeMessage();
}

SessionController::CaptureStatus SessionController::captureStatus() const
{
    CaptureStatus c;
    auto& r = *deps.recorder;
    c.state = r.getState();
    c.mode = r.getStartMode();
    c.barsDone = r.getBarsRecorded();
    c.targetBars = r.getTargetBars();
    c.progress = r.getProgress();
    c.bpm = r.getRecordingBpm();
    c.processing = contextProcessing;
    return c;
}

void SessionController::processCapture (ContextRecorder::Capture&& captured)
{
    contextProcessing = true;
    auto cap = std::make_shared<ContextRecorder::Capture> (std::move (captured));
    const auto folder = deps.contextCacheFolder ? deps.contextCacheFolder() : juce::File();
    const double rate = renderRate();
    loader.post ([this, cap, folder, rate]
    {
        auto asset = std::make_shared<AudioAsset>();
        asset->id = juce::Uuid().toDashedString();
        asset->origin = AudioAsset::Origin::Context;
        asset->audio = std::move (cap->audio);
        asset->sampleRate = cap->sampleRate;
        asset->displayName = "Track context";
        asset->bpm = cap->tempo.bpm;

        const bool hostTempo = cap->mode != ContextRecorder::StartMode::Manual;
        auto analysisResult = analysis::analyze (asset->audio, asset->sampleRate, hostTempo ? std::optional<double> (cap->tempo.bpm) : std::nullopt);
        if (! hostTempo)
            analysisResult.hostBpm = cap->tempo.bpm;   // the tempo the user set is authoritative for the grid

        ContextMeta meta;
        meta.id = asset->id;
        meta.tempo = cap->tempo;
        meta.bars = cap->bars;
        meta.sampleRate = asset->sampleRate;
        meta.lengthSamples = asset->numSamples();
        meta.captured = juce::Time::getCurrentTime();
        meta.startMode = cap->mode == ContextRecorder::StartMode::HostSync ? "host" : cap->mode == ContextRecorder::StartMode::HostTempo ? "signal" : "manual";
        meta.keyDisplay = analysisResult.valid && analysisResult.keyKnown ? analysisResult.keyDisplay() : juce::String();

        juce::String err;
        if (folder != juce::File())
        {
            ContextCache cache (folder);
            if (! cache.store (asset->audio, meta, err))
                logging::warning ("session", "Context cache write failed: " + err);
            cache.prune (keepCaptures, captureCacheBytes, meta.id);
        }
        auto peaks = WaveformPeaks::build (asset->audio, asset->sampleRate);
        auto playback = playbackFor (asset, rate);
        std::optional<TrackAnalysis> a;
        if (analysisResult.valid)
            a = analysisResult;
        AudioAsset::Ptr constAsset = asset;
        onMessageThread ([this, constAsset, playback, peaks, a, meta, err]
        {
            contextProcessing = false;
            applyContext (constAsset, playback, peaks, a, meta, true);
            if (err.isNotEmpty())
                notify ("The capture works now but couldn't be saved for next time (" + err + ").", true);
        });
    });
    sendChangeMessage();
}

void SessionController::applyContext (AudioAsset::Ptr asset, std::shared_ptr<const juce::AudioBuffer<float>> playback, WaveformPeaks::Ptr peaks,
                                      std::optional<TrackAnalysis> a, ContextMeta meta, bool announce)
{
    const auto oldTempo = sessionTempo();
    contextAsset = std::move (asset);
    contextPlayback = std::move (playback);
    contextPeaks = std::move (peaks);
    ctxAnalysis = a;
    contextMissing = false;
    contextMessage = {};
    mutate ([&meta] (SessionState& s) { s.context = meta; });
    if (a)
        deps.analysis->setExternalResult (*a);
    deps.engine->setTempo (meta.tempo.bpm);
    publishProgram();
    if (! (oldTempo == sessionTempo()) || fitted == nullptr)
        requestRender();
    if (announce)
        notify ("Captured " + juce::String (meta.bars) + " bars at " + bpmText (meta.tempo.bpm) + " BPM"
                + (meta.keyDisplay.isNotEmpty() ? juce::String::fromUTF8 (" \xc2\xb7 ") + meta.keyDisplay : juce::String()));
    sendChangeMessage();
}

void SessionController::loadContextFromCache (const ContextMeta& meta)
{
    const auto folder = deps.contextCacheFolder ? deps.contextCacheFolder() : juce::File();
    const double rate = renderRate();
    contextProcessing = true;
    loader.post ([this, meta, folder, rate]
    {
        ContextCache cache (folder);
        auto loaded = cache.load (meta);
        if (! loaded.ok())
        {
            const auto msg = loaded.error;
            logging::warning ("session", "Context reload failed: " + loaded.detail);
            onMessageThread ([this, msg]
            {
                contextProcessing = false;
                contextMissing = true;
                contextMessage = msg;
                sendChangeMessage();
            });
            return;
        }
        auto asset = std::make_shared<AudioAsset> (*loaded.asset);
        asset->bpm = meta.tempo.bpm;
        asset->displayName = "Track context";
        auto result = analysis::analyze (asset->audio, asset->sampleRate, meta.tempo.bpm);
        std::optional<TrackAnalysis> a;
        if (result.valid) a = result;
        auto peaks = WaveformPeaks::build (asset->audio, asset->sampleRate);
        AudioAsset::Ptr constAsset = asset;
        auto playback = playbackFor (constAsset, rate);
        onMessageThread ([this, constAsset, playback, peaks, a, meta]
        {
            contextProcessing = false;
            applyContext (constAsset, playback, peaks, a, meta, false);
        });
    });
}

void SessionController::rebuildContextPlayback()
{
    if (contextAsset == nullptr)
        return;
    auto asset = contextAsset;
    const double rate = renderRate();
    loader.post ([this, asset, rate]
    {
        auto playback = playbackFor (asset, rate);
        onMessageThread ([this, asset, playback]
        {
            if (asset == contextAsset)
            {
                contextPlayback = playback;
                publishProgram();
            }
        });
    });
}

SessionController::LaneInfo SessionController::contextLane() const
{
    LaneInfo l;
    l.origin = AudioAsset::Origin::Context;
    l.name = "Track context";
    l.loading = contextProcessing;
    l.missing = contextMissing;
    l.message = contextMessage;
    if (contextAsset != nullptr)
    {
        l.hasAudio = true;
        l.peaks = contextPeaks;
        l.lengthSamples = contextAsset->numSamples();
        l.sampleRate = contextAsset->sampleRate;
    }
    return l;
}

std::optional<TrackAnalysis> SessionController::contextAnalysis() const { return ctxAnalysis; }
bool SessionController::hasContext() const { return contextAsset != nullptr; }

void SessionController::clearContext()
{
    deps.recorder->cancel();
    contextAsset.reset();
    contextPlayback.reset();
    contextPeaks.reset();
    ctxAnalysis.reset();
    contextMissing = false;
    contextMessage = {};
    mutate ([] (SessionState& s) { s.context.reset(); });
    deps.engine->setTempo (sessionTempo().bpm);
    publishProgram();
    requestRender();
}

void SessionController::retargetContextToHostTempo()
{
    const auto h = hostBpm();
    const auto s = getState();
    if (contextAsset == nullptr || ! h || ! s.context)
        return;
    const musical::Tempo target { *h, deps.host().signature.isValid() ? deps.host().signature : s.context->tempo.signature };
    auto asset = contextAsset;
    const auto oldMeta = *s.context;
    const auto folder = deps.contextCacheFolder ? deps.contextCacheFolder() : juce::File();
    const double rate = renderRate();
    contextProcessing = true;
    notify ("Fitting the captured context to " + bpmText (target.bpm) + " BPM...");
    loader.post ([this, asset, target, oldMeta, folder, rate]
    {
        FitRequest req;
        req.source = asset;
        req.tempo = target;
        req.bars = oldMeta.bars;
        req.outputRate = asset->sampleRate;
        req.settings.mode = FitMode::Stretch;
        auto fit = timefit::render (req);
        if (! fit.ok())
        {
            const auto err = fit.error;
            onMessageThread ([this, err] { contextProcessing = false; notify ("Couldn't retarget the context: " + err, true); });
            return;
        }
        auto fresh = std::make_shared<AudioAsset>();
        fresh->id = juce::Uuid().toDashedString();
        fresh->origin = AudioAsset::Origin::Context;
        fresh->audio = *fit.audio;
        fresh->sampleRate = asset->sampleRate;
        fresh->displayName = "Track context";
        fresh->bpm = target.bpm;
        ContextMeta meta = oldMeta;
        meta.id = fresh->id;
        meta.tempo = target;
        meta.lengthSamples = fresh->numSamples();
        meta.captured = juce::Time::getCurrentTime();
        if (folder != juce::File())
        {
            juce::String err;
            ContextCache (folder).store (fresh->audio, meta, err);
        }
        auto result = analysis::analyze (fresh->audio, fresh->sampleRate, target.bpm);
        std::optional<TrackAnalysis> a;
        if (result.valid) a = result;
        auto peaks = WaveformPeaks::build (fresh->audio, fresh->sampleRate);
        AudioAsset::Ptr constAsset = fresh;
        auto playback = playbackFor (constAsset, rate);
        onMessageThread ([this, constAsset, playback, peaks, a, meta]
        {
            contextProcessing = false;
            applyContext (constAsset, playback, peaks, a, meta, false);
            notify ("Context now plays at " + bpmText (meta.tempo.bpm) + " BPM.");
        });
    });
    sendChangeMessage();
}

// ---------------------------------------------------------------------------
// Output source
// ---------------------------------------------------------------------------

void SessionController::loadGenerated (const GenerationResult& r)
{
    mutate ([&r] (SessionState& s)
    {
        s.sourceKind = SessionState::SourceKind::Generated;
        s.generatedId = r.generationId;
        s.importedPath = {};
        s.restoreGeneratedId = {};
    });
    outputLoading = true;
    outputMissing = false;
    outputMessage = {};
    const auto serial = ++loadSerial;
    const auto result = r;
    loader.post ([this, result, serial]
    {
        auto loaded = assets::loadFile (result.localFile, AudioAsset::Origin::Generated);
        if (! loaded.ok())
        {
            const auto err = loaded.error;
            onMessageThread ([this, err, serial]
            {
                if (serial != loadSerial) return;
                outputLoading = false;
                outputMissing = true;
                outputMessage = err;
                notify ("Couldn't load the generated sound: " + err, true);
            });
            return;
        }
        auto asset = std::make_shared<AudioAsset> (*loaded.asset);
        asset->generationId = result.generationId;
        asset->displayName = result.prompt.isNotEmpty() ? result.prompt : result.localFile.getFileNameWithoutExtension();
        asset->bpm = result.bpm;
        asset->key = result.key;
        asset->mode = result.mode;
        auto analysisResult = stretch::analyseMaterial (asset->audio, asset->sampleRate);
        AudioAsset::Ptr constAsset = asset;
        onMessageThread ([this, constAsset, analysisResult, serial]
        {
            if (serial != loadSerial) return;
            setOutputSource (constAsset, analysisResult);
        });
    });
    sendChangeMessage();
}

void SessionController::importFile (const juce::File& file)
{
    if (! assets::isSupportedAudioFile (file))
    {
        notify (file.existsAsFile() ? "\"" + file.getFileName() + "\" isn't a supported audio file (WAV, AIFF, FLAC, OGG, MP3)."
                                    : "That file couldn't be found.", true);
        return;
    }
    mutate ([&file] (SessionState& s)
    {
        if (s.sourceKind == SessionState::SourceKind::Generated && s.generatedId.isNotEmpty())
            s.restoreGeneratedId = s.generatedId;
        s.sourceKind = SessionState::SourceKind::Imported;
        s.importedPath = file.getFullPathName();
    });
    outputLoading = true;
    outputMissing = false;
    outputMessage = {};
    const auto serial = ++loadSerial;
    loader.post ([this, file, serial]
    {
        auto loaded = assets::loadFile (file, AudioAsset::Origin::Imported);
        if (! loaded.ok())
        {
            const auto err = loaded.error, detail = loaded.detail;
            logging::warning ("session", "Import failed: " + detail);
            onMessageThread ([this, err, serial]
            {
                if (serial != loadSerial) return;
                outputLoading = false;
                outputMissing = true;
                outputMessage = err;
                notify (err, true);
            });
            return;
        }
        auto analysisResult = stretch::analyseMaterial (loaded.asset->audio, loaded.asset->sampleRate);
        auto asset = loaded.asset;
        onMessageThread ([this, asset, analysisResult, serial, file]
        {
            if (serial != loadSerial) return;
            setOutputSource (asset, analysisResult);
            notify ("Loaded " + file.getFileName());
        });
    });
    sendChangeMessage();
}

void SessionController::setOutputSource (AudioAsset::Ptr asset, std::optional<stretch::MaterialAnalysis> m)
{
    source = std::move (asset);
    material = m;
    outputLoading = false;
    outputMissing = false;
    outputMessage = {};
    requestRender();
    sendChangeMessage();
}

void SessionController::clearOutput()
{
    ++loadSerial;
    source.reset();
    material.reset();
    fitted.reset();
    fittedPeaks.reset();
    hasReport = false;
    outputLoading = outputMissing = false;
    outputMessage = {};
    mutate ([] (SessionState& s)
    {
        if (s.sourceKind == SessionState::SourceKind::Generated && s.generatedId.isNotEmpty())
            s.restoreGeneratedId = s.generatedId;
        s.sourceKind = SessionState::SourceKind::None;
        s.importedPath = {};
        s.generatedId = {};
    });
    publishProgram();
}

bool SessionController::canRestoreGenerated() const
{
    const auto s = getState();
    return s.restoreGeneratedId.isNotEmpty() && s.sourceKind != SessionState::SourceKind::Generated;
}

void SessionController::restoreGenerated()
{
    const auto id = getState().restoreGeneratedId;
    if (id.isEmpty() || ! deps.findGenerated)
        return;
    if (auto r = deps.findGenerated (id); r && r->localFile.existsAsFile())
        loadGenerated (*r);
    else
        notify ("The generated sound is no longer in Context's library.", true);
}

SessionController::LaneInfo SessionController::outputLane() const
{
    LaneInfo l;
    l.loading = outputLoading || rendering;
    l.missing = outputMissing;
    l.message = outputMessage;
    if (source != nullptr)
    {
        l.origin = source->origin;
        l.name = source->displayName;
    }
    if (fitted != nullptr)
    {
        l.hasAudio = true;
        l.peaks = fittedPeaks;
        l.lengthSamples = fitted->getNumSamples();
        l.sampleRate = renderRate();
    }
    return l;
}

// ---------------------------------------------------------------------------
// Fit
// ---------------------------------------------------------------------------

void SessionController::setLengthBars (int bars)
{
    if (! musical::isValidLengthBars (bars) || bars == getState().lengthBars)
        return;
    mutate ([bars] (SessionState& s) { s.lengthBars = bars; });
    requestRender();
}

void SessionController::setFitMode (FitMode m)
{
    mutate ([m] (SessionState& s) { s.fit.mode = m; });
    requestRender();
}

void SessionController::setAlgorithm (stretch::Algorithm a)
{
    mutate ([a] (SessionState& s) { s.fit.algorithm = a; });
    requestRender();
}

void SessionController::setPitch (double semitones)
{
    semitones = juce::jlimit (-24.0, 24.0, std::round (semitones * 10.0) / 10.0);
    if (std::abs (semitones - getState().fit.pitchSemitones) < 1.0e-9)
        return;
    mutate ([semitones] (SessionState& s) { s.fit.pitchSemitones = semitones; });
    requestRender();
}

void SessionController::resetFit()
{
    mutate ([] (SessionState& s) { s.fit = FitSettings{}; });
    requestRender();
}

SessionController::FitStatus SessionController::fitStatus() const
{
    FitStatus f;
    f.rendering = rendering;
    f.progress = renderProgress.load();
    f.hasReport = hasReport;
    f.report = report;
    f.error = fitError;
    return f;
}

void SessionController::requestRender()
{
    if (renderToken)
        renderToken->cancel();
    if (source == nullptr)
    {
        rendering = false;
        fitted.reset();
        fittedPeaks.reset();
        hasReport = false;
        publishProgram();
        sendChangeMessage();
        return;
    }

    renderToken = CancellationToken::create();
    const auto serial = ++renderSerial;
    rendering = true;
    renderProgress = 0.0f;
    fitError = {};

    FitRequest req;
    req.source = source;
    req.tempo = sessionTempo();
    req.bars = getState().lengthBars;
    req.outputRate = renderRate();
    req.settings = getState().fit;
    req.material = material;
    auto token = renderToken;

    renderer.clearPending();
    renderer.post ([this, req, token, serial]
    {
        stretch::StretchProgress progress;
        progress.cancel = token.get();
        progress.report = [this] (float p) { renderProgress.store (p); };
        const auto start = juce::Time::getMillisecondCounterHiRes();
        auto result = timefit::render (req, progress);
        if (token->isCancelled())
            return;
        WaveformPeaks::Ptr peaks;
        if (result.ok())
            peaks = WaveformPeaks::build (*result.audio, req.outputRate);
        const auto ms = juce::Time::getMillisecondCounterHiRes() - start;
        onMessageThread ([this, result, peaks, serial, ms]
        {
            if (serial != renderSerial)
                return;   // a newer render superseded this one
            rendering = false;
            renderProgress = 1.0f;
            if (result.ok())
            {
                fitted = result.audio;
                fittedPeaks = peaks;
                report = result.report;
                hasReport = true;
                ++fittedSerial;
                logging::info ("session", "Fit: " + toLabel (report.modeUsed) + (report.engineUsed ? " / " + stretch::toLabel (report.algorithmUsed) : juce::String())
                                          + ", " + juce::String (report.sourceBars, 2) + " -> " + juce::String (report.targetBars) + " bars in "
                                          + juce::String (ms, 0) + " ms");
                publishProgram();
                scheduleExport();
            }
            else
            {
                fitError = result.error;
                notify (result.error, true);
            }
            sendChangeMessage();
        });
    });
    sendChangeMessage();
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

void SessionController::publishProgram()
{
    auto p = std::make_unique<PreviewEngine::Program>();
    p->sampleRate = renderRate();
    if (contextPlayback != nullptr)
        p->context = contextPlayback;
    p->output = fitted;
    p->serial = ++programSerial;
    deps.engine->setProgram (std::move (p));
}

void SessionController::play (PreviewEngine::Mode mode)
{
    mutate ([mode] (SessionState& s) { s.playMode = mode; });
    deps.engine->setMode (mode);
    if (deps.engine->timelineLength() <= 0)
    {
        notify (contextAsset == nullptr && fitted == nullptr ? "Capture your track or load a sound first." : "Still preparing audio...", false);
        return;
    }
    deps.engine->play();
    sendChangeMessage();
}

void SessionController::setPlayMode (PreviewEngine::Mode mode)
{
    mutate ([mode] (SessionState& s) { s.playMode = mode; });
    deps.engine->setMode (mode);
}

void SessionController::stop()
{
    deps.engine->stop();
    sendChangeMessage();
}

void SessionController::togglePlay()
{
    if (isPlaying()) stop();
    else play (getState().playMode);
}

void SessionController::rewind()
{
    deps.engine->rewind();
    sendChangeMessage();
}

bool SessionController::isPlaying() const { return deps.engine->isPlaying(); }

void SessionController::setLoop (bool b)
{
    mutate ([b] (SessionState& s) { s.loop = b; });
    deps.engine->setLooping (b);
    scheduleExport();
}

void SessionController::setLaneGainDb (int lane, float db)
{
    db = juce::jlimit (-60.0f, 12.0f, db);
    mutate ([lane, db] (SessionState& s) { (lane == PreviewEngine::contextLane ? s.contextGainDb : s.outputGainDb) = db; });
    deps.engine->setLaneGainDb (lane, db);
}

void SessionController::setLaneMute (int lane, bool m)
{
    mutate ([lane, m] (SessionState& s) { (lane == PreviewEngine::contextLane ? s.contextMute : s.outputMute) = m; });
    deps.engine->setLaneMute (lane, m);
}

void SessionController::setLaneSolo (int lane, bool on)
{
    mutate ([lane, on] (SessionState& s) { (lane == PreviewEngine::contextLane ? s.contextSolo : s.outputSolo) = on; });
    deps.engine->setLaneSolo (lane, on);
}

void SessionController::setCompareOriginal (bool b)
{
    mutate ([b] (SessionState& s) { s.compareOriginal = b; });
    deps.engine->setCompareOriginal (b);
}

juce::int64 SessionController::playheadSamples() const { return deps.engine->position(); }
juce::int64 SessionController::timelineSamples() const { return deps.engine->timelineLength(); }

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

juce::AudioBuffer<float> SessionController::renderProcessed (const juce::AudioBuffer<float>& src, const juce::var& chainState,
                                                             const fx::EffectRegistry& registry, double sampleRate, double bpm, bool loopWrap)
{
    const int T = src.getNumSamples();
    juce::AudioBuffer<float> out (2, T);
    out.clear();
    if (T <= 0)
        return out;

    fx::EffectChain chain (registry);
    chain.fromVar (chainState);
    constexpr int block = 1024;
    chain.prepare (sampleRate, block);
    const int L = chain.latencySamples();
    // Looped material: run one warm-up pass so reverb/delay tails wrap into the start,
    // exactly like the looping preview. Then skip the chain latency.
    const juce::int64 first = (loopWrap ? (juce::int64) T : 0) + L;
    const juce::int64 total = first + T;

    juce::AudioBuffer<float> work (2, block);
    fx::ProcessContext ctx;
    ctx.bpm = bpm;
    for (juce::int64 pos = 0; pos < total; pos += block)
    {
        const int n = (int) juce::jmin<juce::int64> (block, total - pos);
        for (int i = 0; i < n; ++i)
        {
            const juce::int64 idx = pos + i;
            const bool inRange = loopWrap || idx < T;
            const int s = (int) (idx % T);
            for (int ch = 0; ch < 2; ++ch)
                work.setSample (ch, i, inRange ? src.getSample (juce::jmin (ch, src.getNumChannels() - 1), s) : 0.0f);
        }
        juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), 2, 0, n);
        chain.process (view, n, ctx);
        for (int i = 0; i < n; ++i)
        {
            const juce::int64 o = pos + i - first;
            if (o >= 0 && o < T)
                for (int ch = 0; ch < 2; ++ch)
                    out.setSample (ch, (int) o, work.getSample (ch, i));
        }
    }
    for (int ch = 0; ch < 2; ++ch)
    {
        auto* d = out.getWritePointer (ch);
        for (int i = 0; i < T; ++i)
            if (! std::isfinite (d[i])) d[i] = 0.0f;
    }
    return out;
}

juce::String SessionController::exportKey() const
{
    const auto s = getState();
    juce::String k;
    k << (juce::int64) fittedSerial << "|" << juce::JSON::toString (deps.chain->toVar(), true) << "|"
      << (int) s.compareOriginal << (int) s.loop << "|" << renderRate() << "|" << sessionTempo().bpm;
    return juce::String::toHexString (k.hashCode64()).paddedLeft ('0', 16).substring (0, 8);
}

juce::String SessionController::exportName() const
{
    const auto s = getState();
    juce::String what = source != nullptr ? source->displayName : juce::String ("sound");
    if (source != nullptr && source->origin == AudioAsset::Origin::Imported)
        what = juce::File (source->displayName).getFileNameWithoutExtension();
    juce::StringArray words;
    words.addTokens (what, " _-.", "\"");
    words.removeEmptyStrings();
    while (words.size() > 4) words.remove (words.size() - 1);
    juce::String name;
    name << deps.productName << "_" << words.joinIntoString ("-") << "_" << s.lengthBars << "bars_" << juce::roundToInt (sessionTempo().bpm) << "BPM";
    if (ctxAnalysis && ctxAnalysis->keyKnown)
        name << "_" << filenames::keyForFilename (ctxAnalysis->keyName()) << ctxAnalysis->modeName();
    if (s.compareOriginal)
        name << "_dry";
    return filenames::sanitizeComponent (name, 90);
}

bool SessionController::exportIsCurrent() const
{
    const auto key = exportKey();
    const juce::ScopedLock sl (exportLock);
    return (stagedKey == key && stagedFile.existsAsFile()) || (lastExportKey == key && lastExport.existsAsFile());
}

SessionController::ExportResult SessionController::exportProcessed()
{
    ExportResult r;
    if (fitted == nullptr)
    {
        r.error = rendering ? "Still rendering. Try again in a moment." : "There's nothing to export yet.";
        return r;
    }
    const auto key = exportKey();
    const auto folder = deps.exportFolder ? deps.exportFolder() : juce::File::getSpecialLocation (juce::File::tempDirectory);
    const auto file = folder.getChildFile (exportName() + "_" + key + ".wav");
    if (! file.existsAsFile())
    {
        folder.createDirectory();
        juce::File staged;
        {
            const juce::ScopedLock sl (exportLock);
            if (stagedKey == key && stagedFile.existsAsFile())
                staged = stagedFile;
        }
        if (staged == juce::File() || ! staged.copyFileTo (file))
        {
            const auto s = getState();
            const auto audio = s.compareOriginal ? juce::AudioBuffer<float> (*fitted)
                                                 : renderProcessed (*fitted, deps.chain->toVar(), deps.chain->registry(), renderRate(), sessionTempo().bpm, s.loop);
            juce::String err;
            if (! assets::writeWav (audio, renderRate(), file, 24, err))
            {
                r.error = "Couldn't write the processed file: " + err;
                logging::error ("session", r.error);
                return r;
            }
        }
        logging::info ("session", "Exported " + file.getFileName());
    }
    {
        const juce::ScopedLock sl (exportLock);
        lastExport = file;
        lastExportKey = key;
    }
    r.file = file;
    return r;
}

void SessionController::processingChanged()
{
    scheduleExport();
}

void SessionController::scheduleExport()
{
    exportCountdown = timerHz;   // ~1 s after the last change
}

// ---------------------------------------------------------------------------

void SessionController::notify (const juce::String& text, bool error)
{
    notice.serial++;
    notice.text = text;
    notice.error = error;
    if (error) logging::warning ("session", text);
    else       logging::info ("session", text);
    sendChangeMessage();
}

void SessionController::timerCallback()
{
    deps.engine->collectGarbage();
    deps.chain->collectGarbage();

    // Capture lifecycle (the recorder runs on the audio thread; we poll its state).
    switch (deps.recorder->getState())
    {
        case ContextRecorder::State::Complete:
            if (auto cap = deps.recorder->takeResult())
                processCapture (std::move (*cap));
            break;
        case ContextRecorder::State::Failed:
        {
            const auto f = deps.recorder->getFailure();
            deps.recorder->acknowledgeFailure();
            notify (ContextRecorder::describe (f), true);
            break;
        }
        default:
            break;
    }

    // Host sample-rate change: re-render everything for the new rate.
    const double rate = renderRate();
    if (std::abs (rate - lastRate) > 0.5)
    {
        lastRate = rate;
        rebuildContextPlayback();
        requestRender();
    }

    // Tempo used by tempo-synced effects follows the session tempo.
    deps.engine->setTempo (sessionTempo().bpm);

    // Background pre-render of the processed export (so drag-to-DAW is instant).
    if (exportCountdown > 0 && --exportCountdown == 0 && fitted != nullptr && ! exportIsCurrent())
    {
        exportCountdown = -1;
        if (exporter.numPending() == 0)
        {
            auto src = fitted;
            const auto chainState = deps.chain->toVar();
            const auto key = exportKey();
            const auto s = getState();
            const auto rateNow = renderRate();
            const auto bpm = sessionTempo().bpm;
            const auto folder = deps.stagingFolder ? deps.stagingFolder() : juce::File();
            const auto& registry = deps.chain->registry();
            if (folder != juce::File())
                exporter.post ([this, src, chainState, key, s, rateNow, bpm, folder, &registry]
                {
                    const auto file = folder.getChildFile ("context-staged-" + key + ".wav");
                    const auto audio = s.compareOriginal ? juce::AudioBuffer<float> (*src) : renderProcessed (*src, chainState, registry, rateNow, bpm, s.loop);
                    juce::String err;
                    if (! assets::writeWav (audio, rateNow, file, 24, err))
                        return;
                    juce::File previous;
                    {
                        const juce::ScopedLock sl (exportLock);
                        previous = stagedFile;
                        stagedFile = file;
                        stagedKey = key;
                    }
                    if (previous != file)
                        previous.deleteFile();   // only one temporary pre-render is ever kept
                });
        }
    }
}
} // namespace context
