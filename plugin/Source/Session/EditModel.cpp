#include "EditModel.h"

namespace context::edit
{
namespace
{
    constexpr double eps = 1.0e-9;

    bool finite (double v) { return std::isfinite (v); }

    /** Sorts, clamps to [0, total), drops slivers and resolves any overlap (later start wins). */
    Edit normalised (std::vector<Clip> clips, double total)
    {
        std::vector<Clip> out;
        for (auto c : clips)
        {
            if (! finite (c.start) || ! finite (c.length) || ! finite (c.offset) || ! finite (c.stretch) || c.take.isEmpty())
                continue;
            c.stretch = juce::jlimit (minStretch, maxStretch, c.stretch);
            c.gainDb = juce::jlimit (-60.0f, 12.0f, std::isfinite (c.gainDb) ? c.gainDb : 0.0f);
            if (c.start < 0.0)
            {
                c.offset -= c.start * c.stretch;
                c.length += c.start;
                c.start = 0.0;
            }
            if (c.end() > total)
                c.length = total - c.start;
            if (c.offset < 0.0)
            {
                const double cut = -c.offset / c.stretch;
                c.start += cut;
                c.length -= cut;
                c.offset = 0.0;
            }
            if (c.length < minClipBeats)
                continue;
            out.push_back (c);
        }
        std::stable_sort (out.begin(), out.end(), [] (const Clip& a, const Clip& b) { return a.start < b.start; });
        for (size_t i = 0; i + 1 < out.size(); ++i)
            if (out[i].end() > out[i + 1].start + eps)
                out[i].length = out[i + 1].start - out[i].start;
        out.erase (std::remove_if (out.begin(), out.end(), [] (const Clip& c) { return c.length < minClipBeats; }), out.end());
        return Edit { true, out };
    }

    /** Cuts [from, to) out of a clip list (gaps are silence). */
    std::vector<Clip> carve (const std::vector<Clip>& in, double from, double to)
    {
        std::vector<Clip> out;
        for (const auto& c : in)
        {
            if (c.end() <= from + eps || c.start >= to - eps)
            {
                out.push_back (c);
                continue;
            }
            if (c.start < from)
            {
                auto left = c;
                left.length = from - c.start;
                out.push_back (left);
            }
            if (c.end() > to)
            {
                auto right = c;
                right.offset = c.offset + (to - c.start) * c.stretch;
                right.start = to;
                right.length = c.end() - to;
                out.push_back (right);
            }
        }
        return out;
    }
}

// ---------------------------------------------------------------------------

juce::var Take::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("id", id);
    o->setProperty ("kind", kind == Kind::Imported ? "imported" : "generated");
    o->setProperty ("ref", ref);
    o->setProperty ("name", name);
    return juce::var (o);
}

std::optional<Take> Take::fromVar (const juce::var& v)
{
    if (! v.isObject())
        return std::nullopt;
    Take t;
    t.id = v.getProperty ("id", {}).toString().substring (0, 16);
    t.kind = v.getProperty ("kind", {}).toString() == "imported" ? Kind::Imported : Kind::Generated;
    t.ref = v.getProperty ("ref", {}).toString().substring (0, 1024);
    t.name = v.getProperty ("name", {}).toString().substring (0, 200);
    if (t.id.isEmpty() || ! t.id.containsOnly ("abcdefghijklmnopqrstuvwxyz0123456789") || t.ref.isEmpty())
        return std::nullopt;
    if (t.kind == Kind::Imported && ! juce::File::isAbsolutePath (t.ref))
        return std::nullopt;
    if (t.kind == Kind::Generated && ! t.ref.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_"))
        return std::nullopt;
    return t;
}

juce::var Edit::toVar() const
{
    if (! active)
        return {};
    juce::Array<juce::var> list;
    for (const auto& c : clips)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("take", c.take);
        o->setProperty ("start", c.start);
        o->setProperty ("length", c.length);
        o->setProperty ("offset", c.offset);
        if (std::abs (c.stretch - 1.0) > eps) o->setProperty ("stretch", c.stretch);
        if (std::abs (c.gainDb) > 1.0e-4f)    o->setProperty ("gain", c.gainDb);
        list.add (juce::var (o));
    }
    return list;
}

Edit Edit::fromVar (const juce::var& v, const juce::StringArray& validTakeIds)
{
    if (! v.isArray())
        return {};
    std::vector<Clip> clips;
    if (auto* arr = v.getArray())
        for (const auto& x : *arr)
        {
            if (! x.isObject() || clips.size() >= 256)
                continue;
            Clip c;
            c.take = x.getProperty ("take", {}).toString();
            if (! validTakeIds.contains (c.take))
                continue;
            auto num = [&] (const char* k, double def) { const auto& p = x.getProperty (k, def); return p.isDouble() || p.isInt() || p.isInt64() ? (double) p : def; };
            c.start = num ("start", -1.0);
            c.length = num ("length", 0.0);
            c.offset = num ("offset", 0.0);
            c.stretch = num ("stretch", 1.0);
            c.gainDb = (float) num ("gain", 0.0);
            if (c.start < 0.0 || c.length <= 0.0 || c.offset < 0.0 || c.start > 4096.0)
                continue;
            clips.push_back (c);
        }
    return normalised (clips, 4096.0);
}

// ---------------------------------------------------------------------------

Edit materialise (const Edit& e, const juce::String& base, double total)
{
    if (e.active)
        return normalised (e.clips, total);
    if (base.isEmpty() || total <= 0.0)
        return Edit { true, {} };
    return Edit { true, { Clip { base, 0.0, total, 0.0, 1.0, 0.0f } } };
}

Edit simplify (const Edit& e, const juce::String& base, double total)
{
    auto n = normalised (e.clips, total);
    if (n.clips.size() == 1)
    {
        const auto& c = n.clips[0];
        if (c.take == base && std::abs (c.start) < eps && std::abs (c.length - total) < 1.0e-6 && std::abs (c.offset) < eps
            && std::abs (c.stretch - 1.0) < eps && std::abs (c.gainDb) < 1.0e-4f)
            return {};
    }
    return n;
}

Edit removeRange (const Edit& e, const juce::String& base, double total, double from, double to)
{
    if (! (to > from))
        return e;
    auto m = materialise (e, base, total);
    return simplify (normalised (carve (m.clips, from, to), total), base, total);
}

Edit place (const Edit& e, const juce::String& base, double total, Clip c)
{
    if (c.take.isEmpty() || ! (c.length > 0.0))
        return e;
    auto m = materialise (e, base, total);
    auto clips = carve (m.clips, c.start, c.end());
    clips.push_back (c);
    return simplify (normalised (clips, total), base, total);
}

Edit splitAt (const Edit& e, const juce::String& base, double total, double beat)
{
    auto m = materialise (e, base, total);
    std::vector<Clip> out;
    bool split = false;
    for (const auto& c : m.clips)
    {
        if (beat > c.start + minClipBeats && beat < c.end() - minClipBeats)
        {
            auto left = c, right = c;
            left.length = beat - c.start;
            right.start = beat;
            right.length = c.end() - beat;
            right.offset = c.offset + (beat - c.start) * c.stretch;
            out.push_back (left);
            out.push_back (right);
            split = true;
        }
        else
            out.push_back (c);
    }
    return split ? normalised (out, total) : e;
}

Edit fillFromTake (const Edit& e, const juce::String& base, double total, double from, double to, const juce::String& take)
{
    from = juce::jlimit (0.0, total, from);
    to = juce::jlimit (0.0, total, to);
    if (to - from < minClipBeats || take.isEmpty())
        return e;
    return place (e, base, total, Clip { take, from, to - from, from, 1.0, 0.0f });
}

Edit moveClip (const Edit& e, const juce::String& base, double total, int index, double newStart)
{
    auto m = materialise (e, base, total);
    if (! juce::isPositiveAndBelow (index, (int) m.clips.size()))
        return e;
    auto c = m.clips[(size_t) index];
    m.clips.erase (m.clips.begin() + index);
    c.start = juce::jlimit (0.0, juce::jmax (0.0, total - minClipBeats), newStart);
    auto clips = carve (m.clips, c.start, c.end());
    clips.push_back (c);
    return simplify (normalised (clips, total), base, total);
}

Edit trimClip (const Edit& e, const juce::String& base, double total, int index, bool startEdge, double edge)
{
    auto m = materialise (e, base, total);
    if (! juce::isPositiveAndBelow (index, (int) m.clips.size()))
        return e;
    auto& c = m.clips[(size_t) index];
    const double prevEnd = index > 0 ? m.clips[(size_t) index - 1].end() : 0.0;
    const double nextStart = index + 1 < (int) m.clips.size() ? m.clips[(size_t) index + 1].start : total;
    if (startEdge)
    {
        const double earliest = juce::jmax (prevEnd, c.start - c.offset / c.stretch);
        edge = juce::jlimit (earliest, c.end() - minClipBeats, edge);
        const double delta = edge - c.start;
        c.offset += delta * c.stretch;
        c.length -= delta;
        c.start = edge;
    }
    else
    {
        const double latest = juce::jmin (nextStart, c.start + (total - c.offset) / c.stretch);
        edge = juce::jlimit (c.start + minClipBeats, juce::jmax (c.start + minClipBeats, latest), edge);
        c.length = edge - c.start;
    }
    return simplify (normalised (m.clips, total), base, total);
}

Edit stretchClip (const Edit& e, const juce::String& base, double total, int index, double newEnd)
{
    auto m = materialise (e, base, total);
    if (! juce::isPositiveAndBelow (index, (int) m.clips.size()))
        return e;
    auto c = m.clips[(size_t) index];
    const double src = c.sourceLength();
    const double newLength = juce::jlimit (src / maxStretch, juce::jmin (src / minStretch, total - c.start), newEnd - c.start);
    if (newLength < minClipBeats)
        return e;
    c.stretch = src / newLength;
    c.length = newLength;
    m.clips.erase (m.clips.begin() + index);
    auto clips = carve (m.clips, c.start, c.end());
    clips.push_back (c);
    return simplify (normalised (clips, total), base, total);
}

Edit stretchClipStart (const Edit& e, const juce::String& base, double total, int index, double newStart)
{
    auto m = materialise (e, base, total);
    if (! juce::isPositiveAndBelow (index, (int) m.clips.size()))
        return e;
    auto c = m.clips[(size_t) index];
    const double src = c.sourceLength();
    const double end = c.end();
    const double newLength = juce::jlimit (src / maxStretch, juce::jmin (src / minStretch, end), end - juce::jmax (0.0, newStart));
    if (newLength < minClipBeats)
        return e;
    c.stretch = src / newLength;
    c.length = newLength;
    c.start = end - newLength;
    m.clips.erase (m.clips.begin() + index);
    auto clips = carve (m.clips, c.start, c.end());
    clips.push_back (c);
    return simplify (normalised (clips, total), base, total);
}

Edit setClipGain (const Edit& e, const juce::String& base, double total, int index, float gainDb)
{
    auto m = materialise (e, base, total);
    if (! juce::isPositiveAndBelow (index, (int) m.clips.size()))
        return e;
    m.clips[(size_t) index].gainDb = juce::jlimit (-60.0f, 12.0f, gainDb);
    return simplify (m, base, total);
}

std::vector<Clip> copyRange (const Edit& e, const juce::String& base, double total, double from, double to)
{
    std::vector<Clip> out;
    if (! (to > from))
        return out;
    auto m = materialise (e, base, total);
    for (auto c : m.clips)
    {
        if (c.end() <= from + eps || c.start >= to - eps)
            continue;
        if (c.start < from)
        {
            c.offset += (from - c.start) * c.stretch;
            c.length -= from - c.start;
            c.start = from;
        }
        if (c.end() > to)
            c.length = to - c.start;
        c.start -= from;
        out.push_back (c);
    }
    return out;
}

Edit paste (const Edit& e, const juce::String& base, double total, const std::vector<Clip>& clips, double at)
{
    auto out = e;
    for (auto c : clips)
    {
        c.start += at;
        out = place (out, base, total, c);
    }
    return out;
}

int clipAt (const Edit& e, const juce::String& base, double total, double beat)
{
    auto m = materialise (e, base, total);
    for (int i = 0; i < (int) m.clips.size(); ++i)
        if (beat >= m.clips[(size_t) i].start - eps && beat < m.clips[(size_t) i].end())
            return i;
    return -1;
}

double snap (double beat, double division)
{
    if (! (division > 0.0))
        return beat;
    return std::round (beat / division) * division;
}

// ---------------------------------------------------------------------------

juce::int64 samplesForBeats (double beats, const musical::Tempo& t, double rate)
{
    return (juce::int64) std::llround (beats * 60.0 / t.bpm * rate);
}

juce::AudioBuffer<float> render (const Edit& e, const juce::String& base, const TakeAudio& takes, const musical::Tempo& tempo,
                                 double rate, juce::int64 totalSamples, stretch::Algorithm algorithm)
{
    juce::AudioBuffer<float> out (2, (int) juce::jmax<juce::int64> (0, totalSamples));
    out.clear();
    if (totalSamples <= 0 || ! tempo.isValid() || rate <= 0.0)
        return out;
    const double totalBeats = totalSamples / rate * tempo.bpm / 60.0;
    const auto m = materialise (e, base, totalBeats + 1.0e-6);
    const int fade = juce::jmax (1, (int) std::round (0.0025 * rate));

    for (size_t i = 0; i < m.clips.size(); ++i)
    {
        const auto& c = m.clips[i];
        auto it = takes.find (c.take);
        if (it == takes.end() || it->second == nullptr)
            continue;
        const auto& src = *it->second;
        const juce::int64 s0 = samplesForBeats (c.start, tempo, rate);
        const juce::int64 s1 = juce::jmin (totalSamples, samplesForBeats (c.end(), tempo, rate));
        if (s1 <= s0)
            continue;
        const juce::int64 srcStart = samplesForBeats (c.offset, tempo, rate);

        // Seamless continuations (a plain split) get no crossfade at all, so an untouched
        // split renders bit-identically to the original.
        auto continues = [&] (const Clip& a, const Clip& b)
        {
            return a.take == b.take && std::abs (a.end() - b.start) < 1.0e-6 && std::abs (a.stretch - 1.0) < 1.0e-9
                && std::abs (b.stretch - 1.0) < 1.0e-9 && std::abs ((a.offset + a.length) - b.offset) < 1.0e-6
                && std::abs (a.gainDb - b.gainDb) < 1.0e-4f;
        };
        const bool joinPrev = i > 0 && continues (m.clips[i - 1], c);
        const bool joinNext = i + 1 < m.clips.size() && continues (c, m.clips[i + 1]);
        const bool fadeIn = ! joinPrev && ! (s0 == 0 && srcStart == 0);
        const bool tail = ! joinNext && s1 < totalSamples;      // render 2.5 ms past the end, fading out
        const int len = (int) (s1 - s0) + (tail ? fade : 0);

        // The clip's audio, `len` samples long.
        juce::AudioBuffer<float> seg (2, len);
        seg.clear();
        if (std::abs (c.stretch - 1.0) < 1.0e-9)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                const int sc = juce::jmin (ch, src.getNumChannels() - 1);
                const juce::int64 avail = juce::jmax<juce::int64> (0, juce::jmin<juce::int64> (len, src.getNumSamples() - srcStart));
                if (avail > 0 && srcStart >= 0)
                    seg.copyFrom (ch, 0, src, sc, (int) srcStart, (int) avail);
            }
        }
        else
        {
            const int srcLen = (int) juce::jmax<juce::int64> (1, std::llround (len * c.stretch));
            juce::AudioBuffer<float> piece (2, srcLen);
            piece.clear();
            for (int ch = 0; ch < 2; ++ch)
            {
                const int sc = juce::jmin (ch, src.getNumChannels() - 1);
                const juce::int64 avail = juce::jmax<juce::int64> (0, juce::jmin<juce::int64> (srcLen, src.getNumSamples() - srcStart));
                if (avail > 0 && srcStart >= 0)
                    piece.copyFrom (ch, 0, src, sc, (int) srcStart, (int) avail);
            }
            auto algo = algorithm == stretch::Algorithm::Auto ? stretch::analyseMaterial (piece, rate).suggested : algorithm;
            auto engine = stretch::createEngine (algo);
            engine->prepare (rate, 2);
            engine->setRatio ((double) len / (double) srcLen);
            auto stretched = engine->process (piece);
            for (int ch = 0; ch < 2; ++ch)
                seg.copyFrom (ch, 0, stretched, ch, 0, juce::jmin (len, stretched.getNumSamples()));
        }

        const float gain = juce::Decibels::decibelsToGain (c.gainDb, -60.0f);
        for (int ch = 0; ch < 2; ++ch)
        {
            seg.applyGain (ch, 0, len, gain);
            if (fadeIn)
                seg.applyGainRamp (ch, 0, juce::jmin (fade, len), 0.0f, 1.0f);
            if (tail)
                seg.applyGainRamp (ch, len - fade, fade, 1.0f, 0.0f);
            const int n = (int) juce::jmin<juce::int64> (len, totalSamples - s0);
            out.addFrom (ch, (int) s0, seg, ch, 0, n);
        }
    }
    return out;
}
} // namespace context::edit
