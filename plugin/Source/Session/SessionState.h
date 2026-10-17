#pragma once

#include "ContextCache.h"
#include "EditModel.h"
#include "Audio/TimeFit.h"
#include "Audio/PreviewEngine.h"

namespace context
{
/** Everything about the V2 workspace that is saved in the DAW project.
    Small by design: audio lives in files, only references and settings are stored.
    fromVar() never fails: missing or malformed fields fall back to defaults, so old
    projects (which have none of these fields) load cleanly. */
struct SessionState
{
    enum class SourceKind { None, Generated, Imported };

    int lengthBars = 8;
    double manualBpm = 120.0;
    musical::TimeSignature manualSignature;

    FitSettings fit;

    float contextGainDb = 0.0f, outputGainDb = 0.0f;
    bool contextMute = false, contextSolo = false, outputMute = false, outputSolo = false;
    PreviewEngine::Mode playMode = PreviewEngine::Mode::Both;
    bool loop = true;
    bool compareOriginal = false;

    // The base take, also written in the V2.0 fields so older builds still find the sound.
    SourceKind sourceKind = SourceKind::None;
    juce::String generatedId;        // history id when the output lane holds a generated result
    juce::String importedPath;       // absolute path when it holds a user sample
    juce::String restoreGeneratedId; // generated result replaced by an import ("Restore generated")

    // Takes + arrangement (V2.1). `baseTake` is what plays wherever the edit doesn't say otherwise.
    std::vector<edit::Take> takes;
    juce::String baseTake;
    edit::Edit arrangement;
    double gridBeats = 1.0;          // snap: 4 = bar (4/4), 1 = beat, 0.25 = 1/16, 0 = off

    std::optional<ContextMeta> context;

    // The user's own tempo / key (V2.1.1). Tempo: used instead of the DAW's when no context is
    // captured (with a context, setting a tempo re-reads or re-fits the context itself).
    // Key: used for prompts and shown instead of the detected key. -1 = detect.
    std::optional<double> tempoOverride;
    int keyRoot = -1;
    bool keyMinor = false;

    juce::var toVar() const;
    static SessionState fromVar (const juce::var&);
    bool operator== (const SessionState&) const;

    static constexpr int maxTakes = 12;
    const edit::Take* findTake (const juce::String& id) const;
    const edit::Take* baseTakeInfo() const { return findTake (baseTake); }
    juce::StringArray takeIds() const;
    /** V2.0 state (one source, no takes) -> one take. Idempotent. */
    void migrateLegacySource();
    juce::String nextTakeId() const;
    /** Undo compares only what the user would call "the work" (not transport/A-B state). */
    juce::String undoKey() const;
};

juce::String toId (SessionState::SourceKind);
juce::String toId (PreviewEngine::Mode);
} // namespace context
