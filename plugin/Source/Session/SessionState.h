#pragma once

#include "ContextCache.h"
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

    SourceKind sourceKind = SourceKind::None;
    juce::String generatedId;        // history id when the output lane holds a generated result
    juce::String importedPath;       // absolute path when it holds a user sample
    juce::String restoreGeneratedId; // generated result replaced by an import ("Restore generated")

    std::optional<ContextMeta> context;

    juce::var toVar() const;
    static SessionState fromVar (const juce::var&);
    bool operator== (const SessionState&) const;
};

juce::String toId (SessionState::SourceKind);
juce::String toId (PreviewEngine::Mode);
} // namespace context
