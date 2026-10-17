#pragma once

#include "Audio/AudioAsset.h"
#include "Audio/MusicalTime.h"

namespace context
{
/** What we know about a captured track context. Small: stored in the DAW project
    so the UI can describe the capture even if its audio file is gone. */
struct ContextMeta
{
    juce::String id;                 // file stem in the cache folder
    musical::Tempo tempo;
    int bars = 0;
    double sampleRate = 0.0;
    juce::int64 lengthSamples = 0;
    juce::Time captured;
    juce::String startMode;          // "host" | "signal" | "manual"
    juce::String keyDisplay;         // "A Minor" / "" (cached for display)

    bool isValid() const { return id.isNotEmpty() && tempo.isValid() && bars > 0 && sampleRate > 0.0 && lengthSamples > 0; }
    juce::var toVar() const;
    static std::optional<ContextMeta> fromVar (const juce::var&);
};

/** Captured contexts live as WAV files in %APPDATA%\Context\ContextCache so a DAW
    project only stores a short reference (plugin state stays tiny). Old captures are
    pruned by count and size; a missing file is reported, never fatal. */
class ContextCache
{
public:
    explicit ContextCache (juce::File folder);

    juce::File folder() const { return dir; }
    juce::File fileFor (const juce::String& id) const;

    /** Worker thread. Writes the audio (24-bit WAV) and a JSON sidecar. */
    bool store (const juce::AudioBuffer<float>&, const ContextMeta&, juce::String& error);

    /** Worker thread. */
    assets::LoadResult load (const ContextMeta&) const;

    /** Keeps the newest `keepNewest` captures and at most `maxBytes`; never deletes `protectId`. */
    void prune (int keepNewest, juce::int64 maxBytes, const juce::String& protectId) const;

    static bool isSafeId (const juce::String&);

private:
    juce::File dir;
};
} // namespace context
