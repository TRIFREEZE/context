#include "HistoryStore.h"
#include "Core/SerialWorker.h"
#include "Storage/Logger.h"
#include "Utils/AtomicFile.h"
#include "Utils/Json.h"

namespace context
{
HistoryStore::HistoryStore (juce::File f, SerialWorker& w) : file (std::move (f)), worker (w)
{
    load();
}

HistoryStore::~HistoryStore()
{
    worker.waitUntilIdle (10000);
}

void HistoryStore::load()
{
    std::vector<GenerationResult> loaded;
    if (file.existsAsFile())
    {
        const bool tooBig = file.getSize() > 16 * 1024 * 1024;
        const auto parsed = tooBig ? json::ParseResult{} : json::parseUntrusted (file.loadFileAsString(), 16 * 1024 * 1024, 16);
        auto* arr = parsed.ok ? json::getPath (parsed.value, "entries").getArray() : nullptr;
        if (arr == nullptr)
        {
            recovered = true;
            const auto backup = file.getSiblingFile ("history.corrupt-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".json");
            file.moveFileTo (backup);
            logging::warning ("history", "history.json was unreadable (" + (tooBig ? juce::String ("too large") : parsed.error)
                                         + "); starting fresh. Backup: " + backup.getFileName());
        }
        else
        {
            juce::StringArray seen;
            for (auto& v : *arr)
            {
                if (auto r = GenerationResult::fromVar (v); r && ! seen.contains (r->generationId))
                {
                    seen.add (r->generationId);
                    loaded.push_back (*r);
                }
                if ((int) loaded.size() >= maxEntries)
                    break;
            }
        }
    }
    const juce::ScopedLock sl (lock);
    entries = std::move (loaded);
}

bool HistoryStore::saveNow()
{
    juce::Array<juce::var> arr;
    {
        const juce::ScopedLock sl (lock);
        for (auto& e : entries)
            arr.add (e.toVar());
    }
    auto* o = new juce::DynamicObject();
    o->setProperty ("version", 1);
    o->setProperty ("entries", arr);
    return atomicfile::writeText (file, juce::JSON::toString (juce::var (o)));
}

void HistoryStore::scheduleSave()
{
    if (! worker.post ([this] { saveNow(); }))
        saveNow();
}

void HistoryStore::changed()
{
    scheduleSave();
    sendChangeMessage();
}

void HistoryStore::add (const GenerationResult& r)
{
    {
        const juce::ScopedLock sl (lock);
        entries.erase (std::remove_if (entries.begin(), entries.end(), [&] (auto& e) { return e.generationId == r.generationId; }), entries.end());
        entries.insert (entries.begin(), r);
        // Overflow trimming of metadata only; files are handled by CacheManager.
        while ((int) entries.size() > maxEntries)
        {
            auto it = std::find_if (entries.rbegin(), entries.rend(), [] (auto& e) { return ! e.favorite && ! e.pinned; });
            if (it == entries.rend())
                break;
            entries.erase (std::next (it).base());
        }
    }
    changed();
}

void HistoryStore::setFavorite (const juce::String& id, bool fav)
{
    {
        const juce::ScopedLock sl (lock);
        for (auto& e : entries)
            if (e.generationId == id)
                e.favorite = fav;
    }
    changed();
}

void HistoryStore::setRating (const juce::String& id, int rating, const juce::StringArray& reasons)
{
    rating = juce::jlimit (-1, 1, rating);
    {
        const juce::ScopedLock sl (lock);
        for (auto& e : entries)
            if (e.generationId == id)
            {
                e.metadata.set ("rating", rating);
                juce::Array<juce::var> r;
                for (auto& x : reasons) r.add (x.substring (0, 32));
                if (rating < 0 && ! r.isEmpty())
                    e.metadata.set ("passReasons", r);
                else
                    e.metadata.remove ("passReasons");
                if (rating > 0)
                    e.favorite = true;
            }
    }
    changed();
}

void HistoryStore::setPinned (const juce::String& id)
{
    bool did = false;
    {
        const juce::ScopedLock sl (lock);
        for (auto& e : entries)
            if (e.generationId == id && ! e.pinned)
                e.pinned = did = true;
    }
    if (did)
        changed();
}

void HistoryStore::touch (const juce::String& id)
{
    {
        const juce::ScopedLock sl (lock);
        for (auto& e : entries)
            if (e.generationId == id)
                e.lastUsedMs = juce::Time::currentTimeMillis();
    }
    scheduleSave();   // no UI change needed
}

std::optional<GenerationResult> HistoryStore::remove (const juce::String& id)
{
    std::optional<GenerationResult> removed;
    {
        const juce::ScopedLock sl (lock);
        auto it = std::find_if (entries.begin(), entries.end(), [&] (auto& e) { return e.generationId == id; });
        if (it != entries.end())
        {
            removed = *it;
            entries.erase (it);
        }
    }
    if (removed)
        changed();
    return removed;
}

void HistoryStore::replaceAll (std::vector<GenerationResult> list)
{
    {
        const juce::ScopedLock sl (lock);
        entries = std::move (list);
    }
    changed();
}

std::vector<GenerationResult> HistoryStore::all() const
{
    const juce::ScopedLock sl (lock);
    return entries;
}

std::vector<GenerationResult> HistoryStore::favorites() const
{
    const juce::ScopedLock sl (lock);
    std::vector<GenerationResult> out;
    for (auto& e : entries)
        if (e.favorite)
            out.push_back (e);
    return out;
}

std::vector<GenerationResult> HistoryStore::recent (int max) const
{
    const juce::ScopedLock sl (lock);
    return std::vector<GenerationResult> (entries.begin(), entries.begin() + juce::jmin ((int) entries.size(), max));
}

std::optional<GenerationResult> HistoryStore::find (const juce::String& id) const
{
    const juce::ScopedLock sl (lock);
    for (auto& e : entries)
        if (e.generationId == id)
            return e;
    return std::nullopt;
}

int HistoryStore::size() const
{
    const juce::ScopedLock sl (lock);
    return (int) entries.size();
}
} // namespace context
