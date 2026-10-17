#pragma once

#include "SessionState.h"
#include "Audio/ContextRecorder.h"
#include "Audio/PreviewEngine.h"
#include "Analysis/AnalysisController.h"
#include "Generation/GenerationModels.h"
#include "DSP/EffectChain.h"
#include "Core/SerialWorker.h"
#include <juce_events/juce_events.h>

namespace context
{
/** Owns the V2 workspace: captured context, the output source (generated or
    imported), musical fitting, the processed export and the preview programs.

    Concurrency model
      - Everything public is called on the MESSAGE THREAD (getState() and
        getChainStateForSaving() are also safe from any thread).
      - Audio is held in immutable AudioAsset / shared const buffers. Workers only ever
        build NEW objects; results come back through MessageManager::callAsync and are
        applied on the message thread, guarded by a lifetime token and a serial number
        (a stale render that finishes late is simply dropped).
      - The audio thread never sees this class: it sees PreviewEngine programs and the
        EffectChain snapshot, both handed over lock-free.
      - Workers are declared last, so they are joined before anything they use dies. */
class SessionController : public juce::ChangeBroadcaster, private juce::Timer
{
public:
    struct HostSnapshot
    {
        double sampleRate = 0.0;
        std::optional<double> bpm;
        musical::TimeSignature signature;
        bool playing = false;
    };

    struct Dependencies
    {
        ContextRecorder* recorder = nullptr;
        PreviewEngine* engine = nullptr;
        fx::EffectChain* chain = nullptr;
        AnalysisController* analysis = nullptr;
        std::function<HostSnapshot()> host;
        std::function<std::optional<GenerationResult> (const juce::String& id)> findGenerated;
        std::function<juce::File()> contextCacheFolder;
        std::function<juce::File()> exportFolder;     // permanent: files dragged / saved into projects
        std::function<juce::File()> stagingFolder;    // temporary: background pre-renders (one file kept)
        juce::String productName = "Context";
    };

    enum class TempoSource { Context, Host, Manual };

    struct CaptureStatus
    {
        ContextRecorder::State state = ContextRecorder::State::Idle;
        ContextRecorder::StartMode mode = ContextRecorder::StartMode::Manual;
        int barsDone = 0, targetBars = musical::contextBars;
        double progress = 0.0;
        double bpm = 0.0;
        bool processing = false;      // captured, being analysed and cached
        bool armed() const noexcept     { return state == ContextRecorder::State::Armed; }
        bool recording() const noexcept { return state == ContextRecorder::State::Recording; }
        bool busy() const noexcept      { return armed() || recording() || processing || state == ContextRecorder::State::Cancelling; }
    };

    struct LaneInfo
    {
        bool hasAudio = false;
        bool loading = false;
        bool missing = false;
        juce::String name;              // file name / prompt / "Track context"
        juce::String message;           // why it's missing, etc.
        WaveformPeaks::Ptr peaks;       // of what plays (fitted output / context)
        juce::int64 lengthSamples = 0;
        double sampleRate = 0.0;
        AudioAsset::Origin origin = AudioAsset::Origin::Imported;
    };

    struct FitStatus
    {
        bool rendering = false;
        float progress = 0.0f;
        bool hasReport = false;
        FitReport report;
        juce::String error;
    };

    struct Notice
    {
        int serial = 0;
        juce::String text;
        bool error = false;
    };

    struct ExportResult
    {
        juce::File file;
        juce::String error;
        bool ok() const { return file.existsAsFile(); }
    };

    explicit SessionController (Dependencies);
    ~SessionController() override;

    // ---- state ----
    SessionState getState() const;
    void restore (const SessionState&, const juce::var& chainState);

    // ---- tempo ----
    musical::Tempo sessionTempo() const;
    TempoSource tempoSource() const;
    std::optional<double> hostBpm() const;
    bool hostTempoDiffers() const;          // context captured at a tempo the host no longer has
    void setManualTempo (double bpm);
    double renderRate() const;

    // ---- context ----
    void startCapture();
    void cancelCapture();
    CaptureStatus captureStatus() const;
    LaneInfo contextLane() const;
    std::optional<TrackAnalysis> contextAnalysis() const;
    bool hasContext() const;
    void clearContext();
    void retargetContextToHostTempo();
    /** The captured context for uploading as audio conditioning. Safe from any thread. */
    struct UploadContext { AudioAsset::Ptr asset; int bars = 0; };
    UploadContext contextForUpload() const;

    // ---- output source / takes ----
    /** Makes the generation the base take (the whole lane plays it; edits are cleared). */
    void loadGenerated (const GenerationResult&);
    /** A finished generation: fills the pending selection (requestFill) if there is one,
        otherwise becomes the new base take. Earlier takes stay available for comping. */
    void generationArrived (const GenerationResult&);
    void importFile (const juce::File&);
    void clearOutput();
    bool canRestoreGenerated() const;
    void restoreGenerated();
    LaneInfo outputLane() const;
    AudioAsset::Ptr outputSource() const { return source; }

    struct TakeInfo
    {
        juce::String id, name;
        int number = 0;                 // "Take 3"
        bool isBase = false, loading = false, missing = false, ready = false, used = false;
        WaveformPeaks::Ptr peaks;       // of the take fitted to the lane
    };
    std::vector<TakeInfo> takes() const;
    void useTakeEverywhere (const juce::String& takeId);
    void removeTake (const juce::String& takeId);

    // ---- editing (positions in beats = quarter notes; see edit::) ----
    struct EditView
    {
        bool edited = false;
        std::vector<edit::Clip> clips;  // materialised: what plays, in order
        juce::String baseTake;
        double totalBeats = 0.0;
        double beatsPerBar = 4.0;
        double gridBeats = 1.0;
        std::optional<std::pair<double, double>> pendingFill;
    };
    EditView editView() const;
    void setGrid (double beats);
    void editSplit (double beat);
    void editDelete (double from, double to);
    void editFill (double from, double to, const juce::String& takeId);
    void editMove (int clipIndex, double newStart);
    void editTrim (int clipIndex, bool startEdge, double newEdge);
    void editStretch (int clipIndex, double newEnd);
    void editClipGain (int clipIndex, float gainDb);
    void editCopy (double from, double to);
    void editCut (double from, double to);
    void editPaste (double at);
    void editDuplicate (double from, double to);
    bool canPaste() const { return ! clipboard.empty(); }
    void editReset();
    /** Remembers [from, to) so the next generation fills it (Fill with a new variation). */
    void requestFill (double from, double to);
    void cancelPendingFill();

    // ---- undo / redo (everything that is "the work": fit, edits, takes, context, effects, mix) ----
    bool canUndo() const;
    bool canRedo() const;
    void undo();
    void redo();
    /** Knob drags etc.: changes between begin and end become ONE undo step. */
    void beginGesture();
    void endGesture();

    // ---- fit ----
    void setLengthBars (int bars);
    void setFitMode (FitMode);
    void setAlgorithm (stretch::Algorithm);
    void setPitch (double semitones);
    void resetFit();
    FitStatus fitStatus() const;
    std::optional<stretch::MaterialAnalysis> materialAnalysis() const { return material; }

    // ---- playback ----
    void play (PreviewEngine::Mode);
    void setPlayMode (PreviewEngine::Mode);
    void stop();
    void togglePlay();
    void rewind();
    void seek (juce::int64 timelineSamples);
    bool isPlaying() const;
    void setLoop (bool);
    void setLaneGainDb (int lane, float db);
    void setLaneMute (int lane, bool);
    void setLaneSolo (int lane, bool);
    void setCompareOriginal (bool);
    juce::int64 playheadSamples() const;
    juce::int64 timelineSamples() const;

    // ---- processing / export ----
    /** Call after any effect parameter change (schedules a background re-render of the export). */
    void processingChanged();
    /** Renders exactly what you hear from the output lane (fit + rack, current A/B state)
        to a 24-bit WAV and returns it. Re-uses the cached file when nothing changed. */
    ExportResult exportProcessed();
    bool exportIsCurrent() const;
    static juce::AudioBuffer<float> renderProcessed (const juce::AudioBuffer<float>& fitted, const juce::var& chainState,
                                                     const fx::EffectRegistry&, double sampleRate, double bpm, bool loopWrap);

    Notice lastNotice() const { return notice; }
    void notify (const juce::String& text, bool error = false);

    /** Called by the processor when the DAW sample rate may have changed. */
    void hostConfigurationChanged();

private:
    struct TakeRuntime
    {
        AudioAsset::Ptr asset;
        std::optional<stretch::MaterialAnalysis> material;
        std::shared_ptr<const juce::AudioBuffer<float>> fitted;
        WaveformPeaks::Ptr peaks;
        FitReport report;
        juce::String fitKey;
        bool loading = false, missing = false;
        juce::String message;
        juce::uint64 serial = 0;
    };
    struct Snapshot
    {
        SessionState state;
        juce::var chain;
        juce::String key;
    };

    void timerCallback() override;
    template <typename Fn> void mutate (Fn&& change);
    juce::String addTake (edit::Take::Kind, const juce::String& ref, const juce::String& name);
    void setBaseTake (const juce::String& id, bool clearEdits);
    void ensureTakeLoaded (const edit::Take&);
    void takeLoaded (const juce::String& id, juce::uint64 serial, AudioAsset::Ptr, std::optional<stretch::MaterialAnalysis>, juce::String error);
    void syncTakesRuntime();
    void refreshBaseSource();
    double totalBeats() const;
    template <typename Fn> void editWith (Fn&& op);
    Snapshot currentSnapshot() const;
    void commitHistory();
    void applySnapshot (const Snapshot&);
    void reconcile (const SessionState& before);
    void applyEngineSettings();
    void publishProgram();
    void requestRender();
    void scheduleExport();
    void loadContextFromCache (const ContextMeta&);
    void applyContext (AudioAsset::Ptr, std::shared_ptr<const juce::AudioBuffer<float>> playback, WaveformPeaks::Ptr,
                       std::optional<TrackAnalysis>, ContextMeta, bool announce);
    void rebuildContextPlayback();
    void processCapture (ContextRecorder::Capture&&);
    juce::String exportKey() const;
    juce::String exportName() const;
    template <typename Fn> void onMessageThread (Fn&&);

    Dependencies deps;
    std::shared_ptr<int> life = std::make_shared<int> (0);

    mutable juce::CriticalSection stateLock;
    SessionState state;

    // Context (message thread; uploadContext is a lock-protected copy for worker threads)
    AudioAsset::Ptr contextAsset;
    mutable juce::SpinLock uploadLock;
    UploadContext uploadContext;
    void publishUploadContext();
    std::shared_ptr<const juce::AudioBuffer<float>> contextPlayback;   // at renderRate()
    WaveformPeaks::Ptr contextPeaks;
    std::optional<TrackAnalysis> ctxAnalysis;
    bool contextMissing = false, contextProcessing = false;
    juce::String contextMessage;

    // Takes (message thread). `source`/`material` mirror the base take; `fitted` is the
    // composed output lane (all edits applied), which is what plays and what is exported.
    std::map<juce::String, TakeRuntime> takeData;
    std::vector<edit::Clip> clipboard;
    std::optional<std::pair<double, double>> pendingFill;

    // Undo history (message thread)
    std::vector<Snapshot> undoStack, redoStack;
    Snapshot committed;
    int gestureDepth = 0, quietTicks = 0;
    juce::String lastSeenKey;
    bool applyingHistory = false;

    // Output (message thread)
    AudioAsset::Ptr source;
    std::optional<stretch::MaterialAnalysis> material;
    bool outputLoading = false, outputMissing = false;
    juce::String outputMessage;
    std::shared_ptr<const juce::AudioBuffer<float>> fitted;
    WaveformPeaks::Ptr fittedPeaks;
    FitReport report;
    bool hasReport = false, rendering = false;
    juce::String fitError;
    std::atomic<float> renderProgress { 0.0f };
    juce::uint64 renderSerial = 0, fittedSerial = 0, programSerial = 0, loadSerial = 0;
    CancellationToken::Ptr renderToken;
    double lastRate = 0.0;

    // Export: the latest background pre-render (temporary) and the last real export.
    mutable juce::CriticalSection exportLock;
    juce::File stagedFile, lastExport;
    juce::String stagedKey, lastExportKey;
    int exportCountdown = -1;

    Notice notice;

    SerialWorker loader { "Context assets", 16 };
    SerialWorker renderer { "Context fit", 4 };
    SerialWorker exporter { "Context export", 2 };

    JUCE_DECLARE_NON_COPYABLE (SessionController)
};
} // namespace context
