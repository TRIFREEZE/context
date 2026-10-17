#pragma once

#include "HistoryStore.h"

namespace context
{
/** Bounded storage for generated audio. Deliberately conservative:
    a file is deleted ONLY if all of these are true —
      1. it is tracked in history,
      2. it is not a favorite and not pinned (dragged/exported/copied into a project),
      3. its canonical path is inside the managed "Generated" folder,
      4. its name matches the pattern Context itself creates (<Product>_*.wav/.aiff).
    Never deletes folders, never touches files outside the managed folder,
    never touches exported copies. Least-recently-used files go first. */
class CacheManager
{
public:
    struct Report { int deletedFiles = 0; juce::int64 freedBytes = 0; juce::int64 totalBytes = 0; juce::StringArray problems; };

    static bool isDeletable (const GenerationResult&, const juce::File& managedFolder, const juce::String& productName);

    /** Deletes LRU deletable files until tracked files total <= limitBytes. History entries of deleted files are removed. */
    static Report enforceLimit (HistoryStore&, const juce::File& managedFolder, juce::int64 limitBytes, const juce::String& productName);

    /** "Clear Cache": deletes every deletable file (favorites and pinned files are kept). */
    static Report clear (HistoryStore&, const juce::File& managedFolder, const juce::String& productName);

    /** Removes history entries whose files no longer exist (user deleted them manually). */
    static int pruneMissing (HistoryStore&);

    /** Removes stale *.part and *-context.wav files from the Temp folder (older than maxAge). */
    static int cleanTemp (const juce::File& tempFolder, juce::RelativeTime maxAge);

    static juce::int64 trackedBytes (const HistoryStore&);
};
} // namespace context
