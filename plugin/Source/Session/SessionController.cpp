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
    committed = currentSnapshot();
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

void SessionController::restore (const SessionState& restored, const juce::var& chainState)
{
    auto s = restored;
    s.migrateLegacySource();
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
    publishUploadContext();
    if (s.context)
        loadContextFromCache (*s.context);

    // Takes: (re)load every take; the base take is what the lane shows.
    ++loadSerial;
    takeData.clear();
    source.reset();
    material.reset();
    fitted.reset();
    fittedPeaks.reset();
    hasReport = false;
    outputLoading = outputMissing = false;
    outputMessage = {};
    pendingFill.reset();
    syncTakesRuntime();
    refreshBaseSource();
    publishProgram();

    // A loaded project starts a fresh undo history.
    undoStack.clear();
    redoStack.clear();
    committed = currentSnapshot();
    quietTicks = 0;
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
    const auto s = getState();
    if (s.context)
        return TempoSource::Context;
    if (s.tempoOverride)
        return TempoSource::Set;
    return hostBpm() ? TempoSource::Host : TempoSource::Manual;
}

musical::Tempo SessionController::sessionTempo() const
{
    const auto s = getState();
    if (s.context && s.context->tempo.isValid())
        return s.context->tempo;
    const auto h = deps.host();
    const auto sig = h.signature.isValid() ? h.signature : s.manualSignature;
    if (s.tempoOverride && musical::isValidBpm (*s.tempoOverride))
        return { *s.tempoOverride, sig };
    if (h.bpm && musical::isValidBpm (*h.bpm))
        return { *h.bpm, h.signature.isValid() ? h.signature : musical::TimeSignature{} };
    return { s.manualBpm, s.manualSignature };
}

bool SessionController::hostTempoDiffers() const
{
    const auto s = getState();
    const auto h = hostBpm();
    if (! s.context || ! h)
        return false;
    // Half-time / double-time readings of the DAW tempo play in time with it.
    for (double f : { 1.0, 0.5, 2.0 })
        if (std::abs (*h * f - s.context->tempo.bpm) <= 0.05)
            return false;
    return true;
}

void SessionController::setTempo (double bpm)
{
    const DiscreteStep step (*this);
    if (! musical::isValidBpm (bpm))
        return;
    const auto s = getState();
    if (! s.context)
    {
        mutate ([bpm] (SessionState& st) { st.tempoOverride = bpm; st.manualBpm = bpm; });
        deps.engine->setTempo (bpm);
        requestRender();
        sendChangeMessage();
        return;
    }
    const auto old = s.context->tempo;
    if (std::abs (old.bpm - bpm) < 0.005)
        return;
    // Same audio, read at another tempo: possible when it is still a whole number of bars.
    const double bars = s.context->bars * bpm / old.bpm;
    const int whole = (int) std::lround (bars);
    if (std::abs (bars - whole) < 1.0e-6 && whole >= 1 && whole <= 64)
    {
        auto meta = *s.context;
        meta.tempo.bpm = bpm;
        meta.bars = whole;
        mutate ([&meta] (SessionState& st) { st.context = meta; });
        publishUploadContext();
        deps.engine->setTempo (bpm);
        publishProgram();
        requestRender();
        notify ("Your track is now read as " + juce::String (whole) + " bars at " + bpmText (bpm) + " BPM (same audio).");
        sendChangeMessage();
        return;
    }
    retargetContext ({ bpm, old.signature });
}

void SessionController::clearTempo()
{
    const DiscreteStep step (*this);
    if (! getState().tempoOverride)
        return;
    mutate ([] (SessionState& st) { st.tempoOverride.reset(); });
    deps.engine->setTempo (sessionTempo().bpm);
    requestRender();
    sendChangeMessage();
}

juce::String SessionController::Key::display() const
{
    return known() ? pitchClassName (root) + (minor ? " minor" : " major") : juce::String();
}

SessionController::Key SessionController::detectedKey() const
{
    Key k;
    auto a = ctxAnalysis;
    if (! a && deps.analysis != nullptr)
        a = deps.analysis->getLatest();
    if (a && a->valid && a->keyKnown)
    {
        k.root = a->keyRoot;
        k.minor = a->minor;
        k.confidence = a->keyConfidence;
    }
    return k;
}

SessionController::Key SessionController::sessionKey() const
{
    const auto s = getState();
    if (s.keyRoot >= 0 && s.keyRoot < 12)
    {
        Key k;
        k.root = s.keyRoot;
        k.minor = s.keyMinor;
        k.chosen = true;
        k.confidence = 1.0;
        return k;
    }
    return detectedKey();
}

void SessionController::setKey (int root, bool minor)
{
    const DiscreteStep step (*this);
    root = root >= 0 && root < 12 ? root : -1;
    mutate ([root, minor] (SessionState& st) { st.keyRoot = root; st.keyMinor = root >= 0 && minor; });
    sendChangeMessage();
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
    publishUploadContext();
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

SessionController::UploadContext SessionController::contextForUpload() const
{
    const juce::SpinLock::ScopedLockType sl (uploadLock);
    return uploadContext;
}

void SessionController::publishUploadContext()
{
    UploadContext u;
    u.asset = contextAsset;
    if (const auto s = getState(); s.context)
        u.bars = s.context->bars;
    const juce::SpinLock::ScopedLockType sl (uploadLock);
    uploadContext = std::move (u);
}
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
    publishUploadContext();
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
    retargetContext ({ *h, deps.host().signature.isValid() ? deps.host().signature : s.context->tempo.signature });
}

void SessionController::retargetContext (musical::Tempo target)
{
    const auto s = getState();
    if (contextAsset == nullptr || ! s.context || ! target.isValid())
        return;
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
    pendingFill.reset();
    const auto id = addTake (edit::Take::Kind::Generated, r.generationId, r.prompt);
    setBaseTake (id, true);
}

void SessionController::generationArrived (const GenerationResult& r)
{
    if (! pendingFill)
    {
        loadGenerated (r);
        return;
    }
    const auto range = *pendingFill;
    pendingFill.reset();
    const auto id = addTake (edit::Take::Kind::Generated, r.generationId, r.prompt);
    editWith ([&] (const edit::Edit& e, const juce::String& base, double total)
    {
        return edit::fillFromTake (e, base, total, range.first, range.second, id);
    });
    const auto bpb = musical::quarterNotesPerBar (sessionTempo().signature);
    const int firstBar = (int) std::floor (range.first / bpb + 1.0e-9) + 1;
    const int lastBar = juce::jmax (firstBar, (int) std::ceil (range.second / bpb - 1.0e-9));
    const auto* t = getState().findTake (id);
    notify ((t != nullptr ? "Take " + t->id.substring (1) : juce::String ("The new take"))
            + (lastBar > firstBar ? " now plays bars " + juce::String (firstBar) + juce::String::fromUTF8 ("\xe2\x80\x93") + juce::String (lastBar)
                                  : " now plays bar " + juce::String (firstBar)) + ".");
}

void SessionController::importFile (const juce::File& file)
{
    if (! assets::isSupportedAudioFile (file))
    {
        notify (file.existsAsFile() ? "\"" + file.getFileName() + "\" isn't a supported audio file (WAV, AIFF, FLAC, OGG, MP3)."
                                    : "That file couldn't be found.", true);
        return;
    }
    pendingFill.reset();
    const auto id = addTake (edit::Take::Kind::Imported, file.getFullPathName(), file.getFileName());
    setBaseTake (id, true);
}

juce::String SessionController::addTake (edit::Take::Kind kind, const juce::String& ref, const juce::String& name)
{
    juce::String id;
    mutate ([&] (SessionState& s)
    {
        for (auto& t : s.takes)
            if (t.kind == kind && t.ref == ref)
            {
                if (name.isNotEmpty()) t.name = name;
                id = t.id;
                return;
            }
        edit::Take t;
        t.id = s.nextTakeId();
        t.kind = kind;
        t.ref = ref;
        t.name = name;
        s.takes.push_back (t);
        id = t.id;
        // Keep the list short: drop the oldest takes that nothing uses any more.
        while ((int) s.takes.size() > SessionState::maxTakes)
        {
            auto used = [&s] (const juce::String& tid)
            {
                if (tid == s.baseTake) return true;
                for (const auto& c : s.arrangement.clips) if (c.take == tid) return true;
                return false;
            };
            auto it = std::find_if (s.takes.begin(), s.takes.end(), [&] (const edit::Take& x) { return x.id != id && ! used (x.id); });
            if (it == s.takes.end()) break;
            s.takes.erase (it);
        }
    });
    syncTakesRuntime();
    return id;
}

void SessionController::setBaseTake (const juce::String& id, bool clearEdits)
{
    mutate ([&] (SessionState& s)
    {
        const auto* t = s.findTake (id);
        if (t == nullptr)
            return;
        // "Restore generated" remembers the last generated base when a sample replaces it.
        if (const auto* old = s.findTake (s.baseTake); old != nullptr && old->kind == edit::Take::Kind::Generated
                                                       && t->kind == edit::Take::Kind::Imported)
            s.restoreGeneratedId = old->ref;
        s.baseTake = id;
        s.sourceKind = t->kind == edit::Take::Kind::Imported ? SessionState::SourceKind::Imported : SessionState::SourceKind::Generated;
        s.generatedId = t->kind == edit::Take::Kind::Generated ? t->ref : juce::String();
        s.importedPath = t->kind == edit::Take::Kind::Imported ? t->ref : juce::String();
        if (clearEdits)
            s.arrangement = {};
    });
    syncTakesRuntime();
    refreshBaseSource();
    requestRender();
}

void SessionController::syncTakesRuntime()
{
    const auto s = getState();
    for (auto it = takeData.begin(); it != takeData.end();)
        it = s.findTake (it->first) == nullptr ? takeData.erase (it) : std::next (it);
    for (const auto& t : s.takes)
        ensureTakeLoaded (t);
}

void SessionController::ensureTakeLoaded (const edit::Take& take)
{
    auto& rt = takeData[take.id];
    if (rt.asset != nullptr || rt.loading)
        return;
    rt.loading = true;
    rt.missing = false;
    rt.message = {};
    const auto serial = ++rt.serial;
    const auto id = take.id;

    if (take.kind == edit::Take::Kind::Generated)
    {
        auto r = deps.findGenerated ? deps.findGenerated (take.ref) : std::nullopt;
        if (! r || ! r->localFile.existsAsFile())
        {
            rt.loading = false;
            rt.missing = true;
            rt.message = "The generated sound used here is no longer in Context's library.";
            if (id == getState().baseTake)
                refreshBaseSource();
            return;
        }
        const auto result = *r;
        loader.post ([this, id, serial, result]
        {
            auto loaded = assets::loadFile (result.localFile, AudioAsset::Origin::Generated);
            if (! loaded.ok())
            {
                const auto err = loaded.error;
                onMessageThread ([this, id, serial, err] { takeLoaded (id, serial, nullptr, std::nullopt, err); });
                return;
            }
            auto asset = std::make_shared<AudioAsset> (*loaded.asset);
            asset->generationId = result.generationId;
            asset->displayName = result.prompt.isNotEmpty() ? result.prompt : result.localFile.getFileNameWithoutExtension();
            asset->bpm = result.bpm;
            asset->key = result.key;
            asset->mode = result.mode;
            asset->designedLoop = result.type == GenerationType::Loop;
            asset->alignedToContext = contextmode::isAligned (result.metadata.getWithDefault ("contextMode", juce::var()).toString());
            auto analysisResult = stretch::analyseMaterial (asset->audio, asset->sampleRate);
            AudioAsset::Ptr constAsset = asset;
            onMessageThread ([this, id, serial, constAsset, analysisResult] { takeLoaded (id, serial, constAsset, analysisResult, {}); });
        });
    }
    else
    {
        const juce::File file (take.ref);
        if (! file.existsAsFile())
        {
            rt.loading = false;
            rt.missing = true;
            rt.message = "Sample not found: " + file.getFileName();
            if (id == getState().baseTake)
                refreshBaseSource();
            return;
        }
        loader.post ([this, id, serial, file]
        {
            auto loaded = assets::loadFile (file, AudioAsset::Origin::Imported);
            if (! loaded.ok())
            {
                logging::warning ("session", "Import failed: " + loaded.detail);
                const auto err = loaded.error;
                onMessageThread ([this, id, serial, err] { takeLoaded (id, serial, nullptr, std::nullopt, err); });
                return;
            }
            auto analysisResult = stretch::analyseMaterial (loaded.asset->audio, loaded.asset->sampleRate);
            auto asset = loaded.asset;
            onMessageThread ([this, id, serial, asset, analysisResult] { takeLoaded (id, serial, asset, analysisResult, {}); });
        });
    }
}

void SessionController::takeLoaded (const juce::String& id, juce::uint64 serial, AudioAsset::Ptr asset,
                                    std::optional<stretch::MaterialAnalysis> m, juce::String error)
{
    auto it = takeData.find (id);
    if (it == takeData.end() || it->second.serial != serial)
        return;   // removed or reloaded meanwhile
    auto& rt = it->second;
    rt.loading = false;
    const bool isBase = id == getState().baseTake;
    if (asset == nullptr)
    {
        rt.missing = true;
        rt.message = error;
        if (isBase)
            notify (error, true);
    }
    else
    {
        rt.asset = std::move (asset);
        rt.material = m;
        if (isBase && rt.asset->origin == AudioAsset::Origin::Imported)
            notify ("Loaded " + rt.asset->displayName);
    }
    if (isBase)
        refreshBaseSource();
    requestRender();
}

void SessionController::refreshBaseSource()
{
    const auto base = getState().baseTake;
    auto it = takeData.find (base);
    if (it == takeData.end())
    {
        source.reset();
        material.reset();
        outputLoading = outputMissing = false;
        outputMessage = {};
    }
    else
    {
        source = it->second.asset;
        material = it->second.material;
        outputLoading = it->second.loading;
        outputMissing = it->second.missing;
        outputMessage = it->second.message;
    }
    sendChangeMessage();
}

void SessionController::clearOutput()
{
    ++loadSerial;
    if (renderToken)
        renderToken->cancel();
    pendingFill.reset();
    takeData.clear();
    source.reset();
    material.reset();
    fitted.reset();
    fittedPeaks.reset();
    hasReport = false;
    rendering = false;
    outputLoading = outputMissing = false;
    outputMessage = {};
    mutate ([] (SessionState& s)
    {
        if (s.sourceKind == SessionState::SourceKind::Generated && s.generatedId.isNotEmpty())
            s.restoreGeneratedId = s.generatedId;
        s.sourceKind = SessionState::SourceKind::None;
        s.importedPath = {};
        s.generatedId = {};
        s.takes.clear();
        s.baseTake = {};
        s.arrangement = {};
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

std::vector<SessionController::TakeInfo> SessionController::takes() const
{
    const auto s = getState();
    const auto view = edit::materialise (s.arrangement, s.baseTake, totalBeats());
    std::vector<TakeInfo> out;
    for (const auto& t : s.takes)
    {
        TakeInfo i;
        i.id = t.id;
        i.number = t.id.substring (1).getIntValue();
        i.name = t.name.isNotEmpty() ? t.name : juce::String ("Take ") + juce::String (i.number);
        i.isBase = t.id == s.baseTake;
        if (t.kind == edit::Take::Kind::Generated)
            i.generationId = t.ref;
        for (const auto& c : view.clips) i.used = i.used || c.take == t.id;
        if (auto it = takeData.find (t.id); it != takeData.end())
        {
            i.loading = it->second.loading;
            i.missing = it->second.missing;
            i.ready = it->second.fitted != nullptr;
            i.peaks = it->second.peaks;
        }
        out.push_back (i);
    }
    return out;
}

void SessionController::passTake (const juce::String& takeId)
{
    const DiscreteStep step (*this);
    const auto s = getState();
    if (s.findTake (takeId) == nullptr)
        return;
    if (takeId != s.baseTake)
    {
        removeTake (takeId);
        return;
    }
    juce::String next;
    for (auto it = s.takes.rbegin(); it != s.takes.rend() && next.isEmpty(); ++it)
        if (it->id != takeId)
            next = it->id;
    if (next.isEmpty())
    {
        clearOutput();
        return;
    }
    setBaseTake (next, false);
    removeTake (takeId);
}

void SessionController::useTakeEverywhere (const juce::String& takeId)
{
    const DiscreteStep step (*this);
    if (getState().findTake (takeId) != nullptr)
        setBaseTake (takeId, true);
}

void SessionController::removeTake (const juce::String& takeId)
{
    const DiscreteStep step (*this);
    const auto s = getState();
    if (takeId == s.baseTake || s.findTake (takeId) == nullptr)
        return;
    mutate ([&] (SessionState& st)
    {
        st.takes.erase (std::remove_if (st.takes.begin(), st.takes.end(), [&] (const edit::Take& t) { return t.id == takeId; }), st.takes.end());
        if (st.arrangement.isEdited())
        {
            auto& c = st.arrangement.clips;
            c.erase (std::remove_if (c.begin(), c.end(), [&] (const edit::Clip& x) { return x.take == takeId; }), c.end());
        }
    });
    takeData.erase (takeId);
    requestRender();
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

double SessionController::totalBeats() const
{
    return getState().lengthBars * musical::quarterNotesPerBar (sessionTempo().signature);
}

template <typename Fn>
void SessionController::editWith (Fn&& op)
{
    const DiscreteStep step (*this);
    const auto s = getState();
    if (s.baseTake.isEmpty() && ! s.arrangement.isEdited())
        return;
    auto e = op (s.arrangement, s.baseTake, totalBeats());
    if (e == s.arrangement)
        return;
    mutate ([&e] (SessionState& st) { st.arrangement = e; });
    requestRender();
}

SessionController::EditView SessionController::editView() const
{
    const auto s = getState();
    EditView v;
    v.totalBeats = totalBeats();
    v.beatsPerBar = musical::quarterNotesPerBar (sessionTempo().signature);
    v.edited = s.arrangement.isEdited();
    v.baseTake = s.baseTake;
    v.gridBeats = s.gridBeats;
    v.pendingFill = pendingFill;
    if (s.baseTake.isNotEmpty() || v.edited)
        v.clips = edit::materialise (s.arrangement, s.baseTake, v.totalBeats).clips;
    return v;
}

void SessionController::setGrid (double beats)
{
    const DiscreteStep step (*this);
    beats = juce::jlimit (0.0, 16.0, beats);
    mutate ([beats] (SessionState& s) { s.gridBeats = beats; });
}

void SessionController::editSplit (double beat)
{
    editWith ([beat] (const edit::Edit& e, const juce::String& b, double t) { return edit::splitAt (e, b, t, beat); });
}

void SessionController::editDelete (double from, double to)
{
    editWith ([=] (const edit::Edit& e, const juce::String& b, double t) { return edit::removeRange (e, b, t, from, to); });
}

void SessionController::editFill (double from, double to, const juce::String& take)
{
    editWith ([&] (const edit::Edit& e, const juce::String& b, double t) { return edit::fillFromTake (e, b, t, from, to, take); });
}

void SessionController::editMove (int clip, double start)
{
    editWith ([=] (const edit::Edit& e, const juce::String& b, double t) { return edit::moveClip (e, b, t, clip, start); });
}

void SessionController::editTrim (int clip, bool startEdge, double edge)
{
    editWith ([=] (const edit::Edit& e, const juce::String& b, double t) { return edit::trimClip (e, b, t, clip, startEdge, edge); });
}

void SessionController::editStretch (int clip, double end)
{
    editWith ([=] (const edit::Edit& e, const juce::String& b, double t) { return edit::stretchClip (e, b, t, clip, end); });
}

void SessionController::editStretchStart (int clip, double start)
{
    editWith ([=] (const edit::Edit& e, const juce::String& b, double t) { return edit::stretchClipStart (e, b, t, clip, start); });
}

void SessionController::editClipGain (int clip, float db)
{
    editWith ([=] (const edit::Edit& e, const juce::String& b, double t) { return edit::setClipGain (e, b, t, clip, db); });
}

void SessionController::editCopy (double from, double to)
{
    const auto s = getState();
    auto c = edit::copyRange (s.arrangement, s.baseTake, totalBeats(), from, to);
    if (! c.empty())
        clipboard = std::move (c);
    sendChangeMessage();
}

void SessionController::editCut (double from, double to)
{
    editCopy (from, to);
    editDelete (from, to);
}

void SessionController::editPaste (double at)
{
    if (clipboard.empty())
        return;
    const auto clips = clipboard;
    editWith ([&] (const edit::Edit& e, const juce::String& b, double t) { return edit::paste (e, b, t, clips, at); });
}

void SessionController::editDuplicate (double from, double to)
{
    const auto s = getState();
    const auto clips = edit::copyRange (s.arrangement, s.baseTake, totalBeats(), from, to);
    if (clips.empty())
        return;
    editWith ([&] (const edit::Edit& e, const juce::String& b, double t) { return edit::paste (e, b, t, clips, to); });
}

void SessionController::editReset()
{
    const DiscreteStep step (*this);
    if (! getState().arrangement.isEdited())
        return;
    mutate ([] (SessionState& s) { s.arrangement = {}; });
    requestRender();
}

void SessionController::requestFill (double from, double to)
{
    if (to - from < edit::minClipBeats)
        return;
    pendingFill = std::make_pair (from, to);
    sendChangeMessage();
}

void SessionController::cancelPendingFill()
{
    pendingFill.reset();
    sendChangeMessage();
}

// ---------------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------------

SessionController::Snapshot SessionController::currentSnapshot() const
{
    Snapshot snap;
    snap.state = getState();
    snap.chain = deps.chain->toVar();
    snap.key = snap.state.undoKey() + "|" + juce::JSON::toString (snap.chain, true);
    return snap;
}

void SessionController::commitHistory()
{
    auto now = currentSnapshot();
    quietTicks = 0;
    if (now.key == committed.key)
        return;
    undoStack.push_back (std::move (committed));
    if (undoStack.size() > 200)
        undoStack.erase (undoStack.begin());
    committed = std::move (now);
    redoStack.clear();
    sendChangeMessage();
}

bool SessionController::canUndo() const
{
    return ! undoStack.empty() || currentSnapshot().key != committed.key;
}

bool SessionController::canRedo() const { return ! redoStack.empty(); }

void SessionController::undo()
{
    commitHistory();               // a change still settling becomes its own step first
    if (undoStack.empty())
        return;
    auto target = std::move (undoStack.back());
    undoStack.pop_back();
    redoStack.push_back (std::move (committed));
    committed = target;
    applySnapshot (target);
}

void SessionController::redo()
{
    commitHistory();
    if (redoStack.empty())
        return;
    auto target = std::move (redoStack.back());
    redoStack.pop_back();
    undoStack.push_back (std::move (committed));
    committed = target;
    applySnapshot (target);
}

void SessionController::commitPending()
{
    if (gestureDepth == 0 && ! applyingHistory)
        commitHistory();
}

// A drag starts as its own step: whatever was still settling is committed first.
void SessionController::beginGesture() { if (gestureDepth == 0) commitPending(); ++gestureDepth; }

void SessionController::endGesture()
{
    gestureDepth = juce::jmax (0, gestureDepth - 1);
    if (gestureDepth == 0)
        commitHistory();
}

void SessionController::applySnapshot (const Snapshot& snap)
{
    const juce::ScopedValueSetter<bool> guard (applyingHistory, true);
    if (juce::JSON::toString (deps.chain->toVar(), true) != juce::JSON::toString (snap.chain, true))
        deps.chain->applyState (snap.chain);
    const auto before = getState();
    {
        const juce::ScopedLock sl (stateLock);
        state = snap.state;
        state.playMode = before.playMode;            // transport and A/B are not part of history
        state.compareOriginal = before.compareOriginal;
        state.loop = before.loop;
    }
    pendingFill.reset();
    reconcile (before);
    quietTicks = 0;
}

void SessionController::reconcile (const SessionState& before)
{
    const auto after = getState();
    applyEngineSettings();
    const bool contextChanged = before.context.has_value() != after.context.has_value()
                             || (before.context && after.context && before.context->id != after.context->id);
    if (contextChanged)
    {
        if (after.context)
            loadContextFromCache (*after.context);
        else
        {
            contextAsset.reset();
            contextPlayback.reset();
            contextPeaks.reset();
            ctxAnalysis.reset();
            contextMissing = false;
            contextMessage = {};
            publishUploadContext();
        }
    }
    deps.engine->setTempo (sessionTempo().bpm);
    syncTakesRuntime();
    refreshBaseSource();
    requestRender();
    scheduleExport();
    sendChangeMessage();
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
    const DiscreteStep step (*this);
    if (! musical::isValidLengthBars (bars) || bars == getState().lengthBars)
        return;
    mutate ([bars] (SessionState& s) { s.lengthBars = bars; });
    requestRender();
}

void SessionController::setFitMode (FitMode m)
{
    const DiscreteStep step (*this);
    mutate ([m] (SessionState& s) { s.fit.mode = m; });
    requestRender();
}

void SessionController::setAlgorithm (stretch::Algorithm a)
{
    const DiscreteStep step (*this);
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
    const auto s = getState();
    const auto tempo = sessionTempo();
    const double rate = renderRate();
    const int bars = s.lengthBars;

    // Every loaded take is fitted to the same N bars (cached until something relevant changes),
    // then the arrangement is composed from them.
    struct Job
    {
        juce::String id, key;
        AudioAsset::Ptr asset;
        std::optional<stretch::MaterialAnalysis> material;
        std::shared_ptr<const juce::AudioBuffer<float>> cached;
        WaveformPeaks::Ptr cachedPeaks;
        FitReport cachedReport;
    };
    std::vector<Job> jobs;
    bool anyLoading = false;
    for (const auto& t : s.takes)
    {
        auto it = takeData.find (t.id);
        if (it == takeData.end())
            continue;
        anyLoading = anyLoading || it->second.loading;
        if (it->second.asset == nullptr)
            continue;
        Job j;
        j.id = t.id;
        j.asset = it->second.asset;
        j.material = it->second.material;
        j.key << juce::String::toHexString ((juce::pointer_sized_int) j.asset.get()) << "|" << juce::JSON::toString (s.fit.toVar(), true)
              << "|" << bars << "|" << tempo.bpm << "|" << tempo.signature.numerator << "/" << tempo.signature.denominator << "|" << rate;
        if (j.key == it->second.fitKey && it->second.fitted != nullptr)
        {
            j.cached = it->second.fitted;
            j.cachedPeaks = it->second.peaks;
            j.cachedReport = it->second.report;
        }
        jobs.push_back (std::move (j));
    }

    if (jobs.empty())
    {
        rendering = anyLoading;
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
    auto token = renderToken;
    const auto arrangement = s.arrangement;
    const auto base = s.baseTake;
    const auto fit = s.fit;

    renderer.clearPending();
    renderer.post ([this, jobs, token, serial, arrangement, base, fit, tempo, rate, bars]
    {
        struct Done
        {
            juce::String id, key, error;
            std::shared_ptr<const juce::AudioBuffer<float>> audio;
            WaveformPeaks::Ptr peaks;
            FitReport report;
        };
        std::vector<Done> done;
        edit::TakeAudio audio;
        const auto start = juce::Time::getMillisecondCounterHiRes();
        int index = 0;
        const int toFit = (int) std::count_if (jobs.begin(), jobs.end(), [] (const Job& j) { return j.cached == nullptr; });
        for (const auto& j : jobs)
        {
            if (token->isCancelled())
                return;
            if (j.cached != nullptr)
            {
                audio[j.id] = j.cached;
                done.push_back ({ j.id, j.key, {}, j.cached, j.cachedPeaks, j.cachedReport });
                continue;
            }
            FitRequest req;
            req.source = j.asset;
            req.tempo = tempo;
            req.bars = bars;
            req.outputRate = rate;
            req.settings = fit;
            req.material = j.material;
            stretch::StretchProgress progress;
            progress.cancel = token.get();
            progress.report = [this, index, toFit] (float p) { renderProgress.store ((index + p) / (float) juce::jmax (1, toFit)); };
            auto r = timefit::render (req, progress);
            ++index;
            if (token->isCancelled())
                return;
            if (r.ok())
            {
                audio[j.id] = r.audio;
                done.push_back ({ j.id, j.key, {}, r.audio, WaveformPeaks::build (*r.audio, rate), r.report });
            }
            else
                done.push_back ({ j.id, j.key, r.error, nullptr, nullptr, r.report });
        }

        std::shared_ptr<const juce::AudioBuffer<float>> composed;
        WaveformPeaks::Ptr composedPeaks;
        if (! arrangement.isEdited())
        {
            for (const auto& d : done)
                if (d.id == base && d.audio != nullptr)
                {
                    composed = d.audio;
                    composedPeaks = d.peaks;
                }
        }
        else
        {
            const auto total = musical::samplesForBars (bars, tempo, rate);
            composed = std::make_shared<const juce::AudioBuffer<float>> (edit::render (arrangement, base, audio, tempo, rate, total, fit.algorithm));
            if (token->isCancelled())
                return;
            composedPeaks = WaveformPeaks::build (*composed, rate);
        }
        const auto ms = juce::Time::getMillisecondCounterHiRes() - start;
        onMessageThread ([this, done, composed, composedPeaks, serial, ms, base]
        {
            if (serial != renderSerial)
                return;   // a newer render superseded this one
            rendering = false;
            renderProgress = 1.0f;
            juce::String baseError;
            for (const auto& d : done)
            {
                auto it = takeData.find (d.id);
                if (it == takeData.end())
                    continue;
                if (d.audio != nullptr)
                {
                    const bool firstFit = it->second.fitted == nullptr;
                    it->second.fitted = d.audio;
                    it->second.peaks = d.peaks;
                    it->second.report = d.report;
                    it->second.fitKey = d.key;
                    // A generated take with (almost) nothing in it: say so once, rather than leave
                    // the producer wondering whether playback is broken.
                    if (firstFit && d.audio->getNumSamples() > 0)
                    {
                        float peak = 0.0f;
                        for (int ch = 0; ch < d.audio->getNumChannels(); ++ch)
                            peak = juce::jmax (peak, d.audio->getMagnitude (ch, 0, d.audio->getNumSamples()));
                        const auto* t = getState().findTake (d.id);
                        if (t != nullptr && t->kind == edit::Take::Kind::Generated && peak < 0.004f)   // about -48 dBFS
                            notify ("Take " + d.id.substring (1) + " came back almost silent from the generator. Pass it, or try New take.", true);
                    }
                }
                if (d.id == base)
                {
                    if (d.audio != nullptr)
                    {
                        report = d.report;
                        hasReport = true;
                    }
                    else
                        baseError = d.error;
                }
            }
            fitted = composed;
            fittedPeaks = composedPeaks;
            ++fittedSerial;
            if (hasReport)
                logging::info ("session", "Fit: " + toLabel (report.modeUsed) + (report.engineUsed ? " / " + stretch::toLabel (report.algorithmUsed) : juce::String())
                                          + ", " + juce::String (report.sourceBars, 2) + " -> " + juce::String (report.targetBars) + " bars, "
                                          + juce::String ((int) done.size()) + " take(s) in " + juce::String (ms, 0) + " ms");
            if (baseError.isNotEmpty())
            {
                fitError = baseError;
                notify (baseError, true);
            }
            publishProgram();
            scheduleExport();
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

void SessionController::seek (juce::int64 samples)
{
    deps.engine->seek (samples);
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
    const DiscreteStep step (*this);
    mutate ([lane, m] (SessionState& s) { (lane == PreviewEngine::contextLane ? s.contextMute : s.outputMute) = m; });
    deps.engine->setLaneMute (lane, m);
}

void SessionController::setLaneSolo (int lane, bool on)
{
    const DiscreteStep step (*this);
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
    // Adding, removing, reordering or bypassing an effect is a click: its own undo step now
    // (knob moves keep settling, or are grouped by their gesture).
    juce::String structure;
    for (int i = 0; i < deps.chain->size(); ++i)
        structure << deps.chain->instanceId (i) << (deps.chain->isBypassed (i) ? "b" : "a") << ";";
    if (structure != lastChainStructure)
    {
        lastChainStructure = structure;
        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
            commitPending();
    }
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

    // Undo history: a change becomes a step once things have been quiet for ~0.25 s
    // (or when a gesture such as a knob drag ends).
    if (! applyingHistory && gestureDepth == 0)
    {
        const auto key = currentSnapshot().key;
        if (key != lastSeenKey)
        {
            lastSeenKey = key;
            quietTicks = 0;
        }
        else if (key != committed.key && ++quietTicks >= 8)
            commitHistory();
    }

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
