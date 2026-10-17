#include "ContextCache.h"

namespace context
{
juce::var ContextMeta::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("id", id);
    o->setProperty ("bpm", tempo.bpm);
    o->setProperty ("tsNum", tempo.signature.numerator);
    o->setProperty ("tsDen", tempo.signature.denominator);
    o->setProperty ("bars", bars);
    o->setProperty ("sampleRate", sampleRate);
    o->setProperty ("length", lengthSamples);
    o->setProperty ("captured", captured.toMilliseconds());
    o->setProperty ("startMode", startMode);
    o->setProperty ("key", keyDisplay);
    return juce::var (o);
}

std::optional<ContextMeta> ContextMeta::fromVar (const juce::var& v)
{
    if (! v.isObject())
        return std::nullopt;
    ContextMeta m;
    m.id = v.getProperty ("id", {}).toString().substring (0, 64);
    m.tempo.bpm = (double) v.getProperty ("bpm", 0.0);
    m.tempo.signature = { (int) v.getProperty ("tsNum", 4), (int) v.getProperty ("tsDen", 4) };
    m.bars = (int) v.getProperty ("bars", 0);
    m.sampleRate = (double) v.getProperty ("sampleRate", 0.0);
    m.lengthSamples = (juce::int64) v.getProperty ("length", 0);
    m.captured = juce::Time ((juce::int64) v.getProperty ("captured", 0));
    m.startMode = v.getProperty ("startMode", {}).toString().substring (0, 16);
    m.keyDisplay = v.getProperty ("key", {}).toString().substring (0, 32);
    if (! m.isValid() || ! ContextCache::isSafeId (m.id) || m.bars > 64 || m.lengthSamples > (juce::int64) 384000 * 600)
        return std::nullopt;
    return m;
}

ContextCache::ContextCache (juce::File folder) : dir (std::move (folder)) {}

bool ContextCache::isSafeId (const juce::String& id)
{
    return id.length() >= 8 && id.length() <= 64 && id.containsOnly ("abcdefABCDEF0123456789-");
}

juce::File ContextCache::fileFor (const juce::String& id) const
{
    return dir.getChildFile ("context-" + id + ".wav");
}

bool ContextCache::store (const juce::AudioBuffer<float>& audio, const ContextMeta& meta, juce::String& error)
{
    if (! isSafeId (meta.id))
    {
        error = "invalid capture id";
        return false;
    }
    if (! assets::writeWav (audio, meta.sampleRate, fileFor (meta.id), 24, error))
        return false;
    fileFor (meta.id).withFileExtension ("json").replaceWithText (juce::JSON::toString (meta.toVar()));
    return true;
}

assets::LoadResult ContextCache::load (const ContextMeta& meta) const
{
    if (! isSafeId (meta.id))
    {
        assets::LoadResult r;
        r.error = "The captured context reference is invalid.";
        return r;
    }
    const auto f = fileFor (meta.id);
    if (! f.existsAsFile())
    {
        assets::LoadResult r;
        r.error = "The captured context audio is no longer on this computer. Capture it again.";
        r.detail = "missing " + f.getFullPathName();
        return r;
    }
    auto r = assets::loadFile (f, AudioAsset::Origin::Context);
    if (r.ok() && (r.asset->numSamples() != meta.lengthSamples || std::abs (r.asset->sampleRate - meta.sampleRate) > 0.5))
    {
        assets::LoadResult bad;
        bad.error = "The captured context file doesn't match this project. Capture it again.";
        bad.detail = "length/rate mismatch";
        return bad;
    }
    return r;
}

void ContextCache::prune (int keepNewest, juce::int64 maxBytes, const juce::String& protectId) const
{
    auto files = dir.findChildFiles (juce::File::findFiles, false, "context-*.wav");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
               { return a.getLastModificationTime() > b.getLastModificationTime(); });
    juce::int64 total = 0;
    for (int i = 0; i < files.size(); ++i)
    {
        const auto& f = files.getReference (i);
        total += f.getSize();
        const bool isProtected = protectId.isNotEmpty() && f.getFileNameWithoutExtension() == "context-" + protectId;
        if (! isProtected && (i >= keepNewest || total > maxBytes))
        {
            f.deleteFile();
            f.withFileExtension ("json").deleteFile();
        }
    }
}
} // namespace context
