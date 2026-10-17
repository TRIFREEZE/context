#pragma once

#include "Generation/GenerationModels.h"
#include <juce_events/juce_events.h>

namespace context
{
class SerialWorker;

/** Local generation history (history.json), shared by every Context instance in
    the process. Writes are atomic and happen on the file worker. A corrupt file
    is moved aside and history restarts empty; it never crashes the plugin.
    Contains metadata only — never audio data and never credentials. */
class HistoryStore : public juce::ChangeBroadcaster
{
public:
    HistoryStore (juce::File file, SerialWorker& fileWorker);
    ~HistoryStore() override;   // waits for queued saves that reference this store

    void add (const GenerationResult&);
    void setFavorite (const juce::String& id, bool);
    void setPinned (const juce::String& id);          // file was dragged/exported: protect from cleanup
    void touch (const juce::String& id);              // update LRU time
    std::optional<GenerationResult> remove (const juce::String& id);   // returns the removed entry

    std::vector<GenerationResult> all() const;        // newest first
    std::vector<GenerationResult> favorites() const;
    std::vector<GenerationResult> recent (int max) const;
    std::optional<GenerationResult> find (const juce::String& id) const;
    int size() const;

    bool wasRecoveredFromCorruption() const { return recovered; }
    bool saveNow();

    /** Replace the whole list (used by cache cleanup after deleting files). */
    void replaceAll (std::vector<GenerationResult>);

    static constexpr int maxEntries = 1000;

private:
    void load();
    void scheduleSave();
    void changed();

    juce::File file;
    SerialWorker& worker;
    mutable juce::CriticalSection lock;
    std::vector<GenerationResult> entries;   // newest first
    bool recovered = false;
};
} // namespace context
