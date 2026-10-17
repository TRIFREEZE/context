#include "SessionState.h"

namespace context
{
juce::String toId (SessionState::SourceKind k)
{
    switch (k)
    {
        case SessionState::SourceKind::Generated: return "generated";
        case SessionState::SourceKind::Imported:  return "imported";
        case SessionState::SourceKind::None:      break;
    }
    return "none";
}

juce::String toId (PreviewEngine::Mode m)
{
    switch (m)
    {
        case PreviewEngine::Mode::Context: return "context";
        case PreviewEngine::Mode::Output:  return "output";
        case PreviewEngine::Mode::Both:    break;
    }
    return "both";
}

juce::var SessionState::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("v", 2);
    o->setProperty ("bars", lengthBars);
    o->setProperty ("manualBpm", manualBpm);
    o->setProperty ("manualTsNum", manualSignature.numerator);
    o->setProperty ("manualTsDen", manualSignature.denominator);
    o->setProperty ("fit", fit.toVar());
    o->setProperty ("ctxGain", contextGainDb);
    o->setProperty ("outGain", outputGainDb);
    o->setProperty ("ctxMute", contextMute);
    o->setProperty ("ctxSolo", contextSolo);
    o->setProperty ("outMute", outputMute);
    o->setProperty ("outSolo", outputSolo);
    o->setProperty ("playMode", toId (playMode));
    o->setProperty ("loop", loop);
    o->setProperty ("compare", compareOriginal);
    o->setProperty ("source", toId (sourceKind));
    o->setProperty ("generatedId", generatedId);
    o->setProperty ("importedPath", importedPath);
    o->setProperty ("restoreId", restoreGeneratedId);
    if (context)
        o->setProperty ("context", context->toVar());
    if (! takes.empty())
    {
        juce::Array<juce::var> t;
        for (const auto& x : takes) t.add (x.toVar());
        o->setProperty ("takes", t);
        o->setProperty ("baseTake", baseTake);
    }
    if (arrangement.isEdited())
        o->setProperty ("edit", arrangement.toVar());
    o->setProperty ("grid", gridBeats);
    if (tempoOverride)
        o->setProperty ("tempoSet", *tempoOverride);
    if (keyRoot >= 0)
    {
        o->setProperty ("keyRoot", keyRoot);
        o->setProperty ("keyMinor", keyMinor);
    }
    return juce::var (o);
}

SessionState SessionState::fromVar (const juce::var& v)
{
    SessionState s;
    if (! v.isObject())
        return s;
    auto num = [&] (const char* key, double def, double lo, double hi)
    {
        const auto& x = v.getProperty (key, def);
        const double d = (x.isDouble() || x.isInt() || x.isInt64()) ? (double) x : def;
        return std::isfinite (d) ? juce::jlimit (lo, hi, d) : def;
    };
    auto flag = [&] (const char* key, bool def)
    {
        const auto& x = v.getProperty (key, def);
        return x.isBool() || x.isInt() ? (bool) x : def;
    };

    const int bars = (int) num ("bars", 8, 1, 64);
    s.lengthBars = musical::isValidLengthBars (bars) ? bars : 8;
    s.manualBpm = num ("manualBpm", 120.0, musical::minBpm, musical::maxBpm);
    s.manualSignature = { (int) num ("manualTsNum", 4, 1, 32), (int) num ("manualTsDen", 4, 1, 32) };
    if (! s.manualSignature.isValid())
        s.manualSignature = {};
    s.fit = FitSettings::fromVar (v.getProperty ("fit", {}));
    s.contextGainDb = (float) num ("ctxGain", 0.0, -60.0, 12.0);
    s.outputGainDb = (float) num ("outGain", 0.0, -60.0, 12.0);
    s.contextMute = flag ("ctxMute", false);
    s.contextSolo = flag ("ctxSolo", false);
    s.outputMute = flag ("outMute", false);
    s.outputSolo = flag ("outSolo", false);
    const auto mode = v.getProperty ("playMode", "both").toString();
    s.playMode = mode == "context" ? PreviewEngine::Mode::Context : mode == "output" ? PreviewEngine::Mode::Output : PreviewEngine::Mode::Both;
    s.loop = flag ("loop", true);
    s.compareOriginal = flag ("compare", false);
    const auto src = v.getProperty ("source", "none").toString();
    s.sourceKind = src == "generated" ? SourceKind::Generated : src == "imported" ? SourceKind::Imported : SourceKind::None;
    s.generatedId = v.getProperty ("generatedId", {}).toString().substring (0, 64);
    s.importedPath = v.getProperty ("importedPath", {}).toString().substring (0, 1024);
    s.restoreGeneratedId = v.getProperty ("restoreId", {}).toString().substring (0, 64);
    if (s.sourceKind == SourceKind::Imported && ! juce::File::isAbsolutePath (s.importedPath))
    {
        s.sourceKind = SourceKind::None;
        s.importedPath = {};
    }
    s.context = ContextMeta::fromVar (v.getProperty ("context", {}));

    if (auto* arr = v.getProperty ("takes", {}).getArray())
        for (const auto& x : *arr)
            if (auto t = edit::Take::fromVar (x); t && (int) s.takes.size() < maxTakes && s.findTake (t->id) == nullptr)
                s.takes.push_back (*t);
    s.baseTake = v.getProperty ("baseTake", {}).toString();
    s.migrateLegacySource();
    s.arrangement = edit::Edit::fromVar (v.getProperty ("edit", {}), s.takeIds());
    s.gridBeats = num ("grid", 1.0, 0.0, 16.0);
    if (v.hasProperty ("tempoSet"))
        s.tempoOverride = num ("tempoSet", 120.0, musical::minBpm, musical::maxBpm);
    s.keyRoot = (int) num ("keyRoot", -1, -1, 11);
    s.keyMinor = flag ("keyMinor", false);
    return s;
}

bool SessionState::operator== (const SessionState& o) const
{
    return juce::JSON::toString (toVar()) == juce::JSON::toString (o.toVar());
}

const edit::Take* SessionState::findTake (const juce::String& id) const
{
    for (const auto& t : takes)
        if (t.id == id)
            return &t;
    return nullptr;
}

void SessionState::migrateLegacySource()
{
    // Projects from V2.0 (no takes): the single source becomes take 1.
    if (takes.empty() && sourceKind != SourceKind::None)
    {
        edit::Take t;
        t.id = "t1";
        t.kind = sourceKind == SourceKind::Imported ? edit::Take::Kind::Imported : edit::Take::Kind::Generated;
        t.ref = sourceKind == SourceKind::Imported ? importedPath : generatedId;
        t.name = sourceKind == SourceKind::Imported ? juce::File (importedPath).getFileName() : juce::String();
        if (t.ref.isNotEmpty())
        {
            takes.push_back (t);
            baseTake = t.id;
        }
    }
    if (findTake (baseTake) == nullptr)
        baseTake = takes.empty() ? juce::String() : takes.front().id;
}

juce::StringArray SessionState::takeIds() const
{
    juce::StringArray ids;
    for (const auto& t : takes) ids.add (t.id);
    return ids;
}

juce::String SessionState::nextTakeId() const
{
    int n = 1;
    for (const auto& t : takes)
        n = juce::jmax (n, t.id.substring (1).getIntValue() + 1);
    return "t" + juce::String (n);
}

juce::String SessionState::undoKey() const
{
    auto copy = *this;
    copy.playMode = PreviewEngine::Mode::Both;
    copy.compareOriginal = false;
    copy.loop = true;
    return juce::JSON::toString (copy.toVar(), true);
}
} // namespace context
