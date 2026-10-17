#include "CacheManager.h"
#include "Storage/Logger.h"
#include "Utils/FilenameSanitizer.h"

namespace context
{
bool CacheManager::isDeletable (const GenerationResult& r, const juce::File& folder, const juce::String& productName)
{
    if (r.favorite || r.pinned)
        return false;
    if (folder == juce::File() || ! folder.isDirectory())
        return false;
    const auto f = r.localFile;
    if (! filenames::isInsideDirectory (f, folder))
        return false;
    if (f.getParentDirectory() != folder)           // only direct children: no nested folders
        return false;
    const auto name = f.getFileName();
    const auto ext = f.getFileExtension().toLowerCase();
    if (! name.startsWith (filenames::sanitizeComponent (productName, 32) + "_") || (ext != ".wav" && ext != ".aiff"))
        return false;
    return f.existsAsFile();
}

juce::int64 CacheManager::trackedBytes (const HistoryStore& history)
{
    juce::int64 total = 0;
    for (auto& e : history.all())
        if (e.localFile.existsAsFile())
            total += e.localFile.getSize();
    return total;
}

CacheManager::Report CacheManager::enforceLimit (HistoryStore& history, const juce::File& folder, juce::int64 limitBytes, const juce::String& productName)
{
    Report rep;
    auto entries = history.all();
    rep.totalBytes = 0;
    for (auto& e : entries)
        if (e.localFile.existsAsFile())
            rep.totalBytes += e.localFile.getSize();
    if (rep.totalBytes <= limitBytes)
        return rep;

    // Oldest-used first.
    std::vector<size_t> order (entries.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort (order.begin(), order.end(), [&] (size_t a, size_t b)
    {
        const auto ta = juce::jmax (entries[a].lastUsedMs, entries[a].creationTimestamp.toMilliseconds());
        const auto tb = juce::jmax (entries[b].lastUsedMs, entries[b].creationTimestamp.toMilliseconds());
        return ta < tb;
    });

    juce::StringArray removedIds;
    for (auto idx : order)
    {
        if (rep.totalBytes <= limitBytes)
            break;
        auto& e = entries[idx];
        if (! isDeletable (e, folder, productName))
            continue;
        const auto size = e.localFile.getSize();
        if (e.localFile.deleteFile())
        {
            rep.totalBytes -= size;
            rep.freedBytes += size;
            ++rep.deletedFiles;
            removedIds.add (e.generationId);
        }
        else
        {
            rep.problems.add ("Could not delete " + e.localFile.getFileName());
        }
    }

    if (! removedIds.isEmpty())
    {
        entries.erase (std::remove_if (entries.begin(), entries.end(), [&] (auto& e) { return removedIds.contains (e.generationId); }), entries.end());
        history.replaceAll (entries);
        logging::info ("cache", "Cache limit: removed " + juce::String (rep.deletedFiles) + " file(s), freed "
                                + juce::String (rep.freedBytes / (1024 * 1024)) + " MB");
    }
    return rep;
}

CacheManager::Report CacheManager::clear (HistoryStore& history, const juce::File& folder, const juce::String& productName)
{
    Report rep;
    auto entries = history.all();
    juce::StringArray removedIds;
    for (auto& e : entries)
    {
        if (! isDeletable (e, folder, productName))
            continue;
        const auto size = e.localFile.getSize();
        if (e.localFile.deleteFile())
        {
            rep.freedBytes += size;
            ++rep.deletedFiles;
            removedIds.add (e.generationId);
        }
    }
    entries.erase (std::remove_if (entries.begin(), entries.end(), [&] (auto& e) { return removedIds.contains (e.generationId); }), entries.end());
    history.replaceAll (entries);
    rep.totalBytes = trackedBytes (history);
    logging::info ("cache", "Clear cache: removed " + juce::String (rep.deletedFiles) + " file(s)");
    return rep;
}

int CacheManager::pruneMissing (HistoryStore& history)
{
    auto entries = history.all();
    const auto before = entries.size();
    entries.erase (std::remove_if (entries.begin(), entries.end(), [] (auto& e) { return ! e.localFile.existsAsFile(); }), entries.end());
    const int removed = (int) (before - entries.size());
    if (removed > 0)
        history.replaceAll (entries);
    return removed;
}

int CacheManager::cleanTemp (const juce::File& temp, juce::RelativeTime maxAge)
{
    if (! temp.isDirectory())
        return 0;
    int n = 0;
    const auto cutoff = juce::Time::getCurrentTime() - maxAge;
    for (const auto& f : temp.findChildFiles (juce::File::findFiles, false, "*.part;*-context.wav"))
        if (f.getLastModificationTime() < cutoff && f.getParentDirectory() == temp && f.deleteFile())
            ++n;
    return n;
}
} // namespace context
