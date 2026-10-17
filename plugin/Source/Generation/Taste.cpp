#include "Taste.h"

namespace context::taste
{
const std::vector<Reason>& reasons()
{
    static const std::vector<Reason> list {
        { "timing",  "Timing",      "tight timing, locked to the grid, steady tempo", "loose timing, rubato, tempo changes" },
        { "key",     "Off key",     "strictly in key, consonant",                     "off-key, detuned, atonal" },
        { "busy",    "Too busy",    "sparse, minimal, space between notes",           "busy, cluttered, dense" },
        { "empty",   "Too empty",   "full, rich, layered",                            "thin, empty" },
        { "sound",   "Wrong sound", "exactly the sound described",                    "generic, unrelated instruments" },
        { "quality", "Quality",     "clean, high fidelity, polished mix",             "low quality, distorted, noisy, muffled" },
    };
    return list;
}

const Reason* findReason (const juce::String& id)
{
    for (auto& r : reasons())
        if (id == r.id)
            return &r;
    return nullptr;
}

namespace
{
    juce::String normalise (const juce::String& p)
    {
        return p.toLowerCase().replaceCharacters ("\t\r\n", "   ").trim().removeCharacters (".,!?;:");
    }
}

Steer forPrompt (const std::vector<GenerationResult>& history, const juce::String& prompt, GenerationType type)
{
    Steer s;
    const auto key = normalise (prompt);
    if (key.isEmpty())
        return s;
    std::map<juce::String, int> counts;
    int seen = 0;
    for (const auto& r : history)       // newest first
    {
        if (r.type != type || normalise (r.prompt) != key)
            continue;
        const int rating = ratingOf (r);
        if (rating == 0)
            continue;
        if (++seen > 40)
            break;
        if (rating > 0)
        {
            ++s.keeps;          // a keep doesn't undo the steer: the steer helped make it
            continue;
        }
        ++s.passes;
        if (auto* arr = r.metadata.getWithDefault ("passReasons", {}).getArray())
            for (auto& x : *arr)
                if (findReason (x.toString()) != nullptr)
                    ++counts[x.toString()];
    }
    std::vector<std::pair<juce::String, int>> ordered (counts.begin(), counts.end());
    std::stable_sort (ordered.begin(), ordered.end(), [] (auto& a, auto& b) { return a.second > b.second; });
    for (auto& [id, n] : ordered)
    {
        juce::ignoreUnused (n);
        const auto* r = findReason (id);
        // "Too busy" and "Too empty" contradict each other: the more recent / frequent one wins.
        if ((id == "busy" && s.labels.contains ("Too empty")) || (id == "empty" && s.labels.contains ("Too busy")))
            continue;
        s.labels.add (r->label);
        s.hints.add (r->steer);
        s.avoid.addTokens (r->avoid, ",", "");
    }
    s.avoid.trim();
    s.avoid.removeEmptyStrings();
    s.avoid.removeDuplicates (true);
    return s;
}

juce::String exportRatings (const std::vector<GenerationResult>& history)
{
    juce::Array<juce::var> rows;
    for (const auto& r : history)
    {
        const int rating = ratingOf (r);
        if (rating == 0)
            continue;
        auto* o = new juce::DynamicObject();
        o->setProperty ("generationId", r.generationId);
        o->setProperty ("created", r.creationTimestamp.toISO8601 (true));
        o->setProperty ("prompt", r.prompt);
        o->setProperty ("sentPrompt", r.expandedPrompt);
        o->setProperty ("type", toId (r.type));
        if (r.bpm) o->setProperty ("bpm", *r.bpm);
        if (r.key.isNotEmpty()) o->setProperty ("key", r.key + " " + r.mode);
        o->setProperty ("verdict", rating > 0 ? "kept" : "passed");
        o->setProperty ("reasons", r.metadata.getWithDefault ("passReasons", juce::Array<juce::var>()));
        if (r.metadata.contains ("contextMode")) o->setProperty ("trackMode", r.metadata["contextMode"]);
        o->setProperty ("provider", r.providerId);
        o->setProperty ("file", r.localFile.getFullPathName());
        rows.add (juce::var (o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("format", "context-ratings-v1");
    root->setProperty ("exported", juce::Time::getCurrentTime().toISO8601 (true));
    root->setProperty ("ratings", rows);
    return juce::JSON::toString (juce::var (root), false);
}
} // namespace context::taste
