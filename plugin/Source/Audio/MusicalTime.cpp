#include "MusicalTime.h"
#include <cmath>

namespace context::musical
{
double quarterNotesPerBar (TimeSignature ts) noexcept
{
    if (! ts.isValid())
        ts = {};
    return ts.numerator * 4.0 / ts.denominator;
}

double secondsForBars (double bars, const Tempo& t) noexcept
{
    if (! t.isValid() || ! std::isfinite (bars) || bars <= 0.0)
        return 0.0;
    return bars * quarterNotesPerBar (t.signature) * 60.0 / t.bpm;
}

juce::int64 samplesForBars (double bars, const Tempo& t, double sampleRate) noexcept
{
    if (! t.isValid() || ! std::isfinite (bars) || bars <= 0.0 || ! (sampleRate > 0.0))
        return 0;
    // One multiplication chain and one rounding: no per-bar accumulation.
    return std::llround (bars * quarterNotesPerBar (t.signature) * 60.0 * sampleRate / t.bpm);
}

double samplesPerBar (const Tempo& t, double sampleRate) noexcept
{
    if (! t.isValid() || ! (sampleRate > 0.0))
        return 0.0;
    return quarterNotesPerBar (t.signature) * 60.0 * sampleRate / t.bpm;
}

double barsForSamples (juce::int64 samples, const Tempo& t, double sampleRate) noexcept
{
    const double spb = samplesPerBar (t, sampleRate);
    return spb > 0.0 ? (double) samples / spb : 0.0;
}

double barsForSeconds (double seconds, const Tempo& t) noexcept
{
    const double barSeconds = secondsForBars (1.0, t);
    return barSeconds > 0.0 ? seconds / barSeconds : 0.0;
}

double nextBarStartPpq (double ppq, TimeSignature ts, std::optional<double> lastBarStartPpq, double epsilon) noexcept
{
    const double qpb = quarterNotesPerBar (ts);
    if (lastBarStartPpq.has_value() && std::isfinite (*lastBarStartPpq) && *lastBarStartPpq <= ppq + epsilon)
    {
        const double since = ppq - *lastBarStartPpq;
        if (since <= epsilon)
            return *lastBarStartPpq;
        // The host's bar start can lag by whole bars in some hosts; step forward to the next line.
        return *lastBarStartPpq + std::ceil ((since - epsilon) / qpb) * qpb;
    }
    const double barIndex = std::ceil ((ppq - epsilon) / qpb);
    return barIndex * qpb;
}

double bpmForLoop (double seconds, double bars, TimeSignature ts) noexcept
{
    if (! (seconds > 0.0) || ! (bars > 0.0))
        return 0.0;
    return bars * quarterNotesPerBar (ts) * 60.0 / seconds;
}

std::optional<double> bpmFromFileName (const juce::String& name)
{
    // "130bpm", "130 BPM", "130-bpm", "bpm130", "BPM_130"
    const auto lower = name.toLowerCase();
    const int idx = lower.indexOf ("bpm");
    if (idx >= 0)
    {
        // digits before "bpm"
        int end = idx;
        while (end > 0 && (lower[end - 1] == ' ' || lower[end - 1] == '_' || lower[end - 1] == '-'))
            --end;
        int start = end;
        while (start > 0 && (juce::CharacterFunctions::isDigit (lower[start - 1]) || lower[start - 1] == '.'))
            --start;
        double v = start < end ? lower.substring (start, end).getDoubleValue() : 0.0;
        if (! isValidBpm (v) || v < 40.0 || v > 300.0)
        {
            // digits after "bpm"
            int s2 = idx + 3;
            while (s2 < lower.length() && (lower[s2] == ' ' || lower[s2] == '_' || lower[s2] == '-'))
                ++s2;
            int e2 = s2;
            while (e2 < lower.length() && (juce::CharacterFunctions::isDigit (lower[e2]) || lower[e2] == '.'))
                ++e2;
            v = e2 > s2 ? lower.substring (s2, e2).getDoubleValue() : 0.0;
        }
        if (v >= 40.0 && v <= 300.0)
            return v;
    }
    return std::nullopt;
}
} // namespace context::musical
