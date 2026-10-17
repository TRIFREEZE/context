#include "StretchEngines.h"
#include "Audio/Resampler.h"
#include <juce_dsp/juce_dsp.h>
#include <vector>

namespace context::stretch
{
juce::String toId (Algorithm a)
{
    switch (a)
    {
        case Algorithm::Auto:       return "auto";
        case Algorithm::Percussive: return "percussive";
        case Algorithm::Monophonic: return "monophonic";
        case Algorithm::Polyphonic: return "polyphonic";
        case Algorithm::Texture:    return "texture";
        case Algorithm::Repitch:    return "repitch";
    }
    return "auto";
}

juce::String toLabel (Algorithm a)
{
    switch (a)
    {
        case Algorithm::Auto:       return "Auto";
        case Algorithm::Percussive: return "Percussive";
        case Algorithm::Monophonic: return "Monophonic";
        case Algorithm::Polyphonic: return "Polyphonic";
        case Algorithm::Texture:    return "Texture";
        case Algorithm::Repitch:    return "Repitch";
    }
    return "Auto";
}

juce::String tooltipFor (Algorithm a)
{
    switch (a)
    {
        case Algorithm::Auto:       return "Analyses the sound and picks the best mode.";
        case Algorithm::Percussive: return "Preserves transients when fitting drums and rhythmic material.";
        case Algorithm::Monophonic: return "For vocals, bass and leads: single notes stay clean.";
        case Algorithm::Polyphonic: return "Optimized for chords, instruments and complex harmonic material.";
        case Algorithm::Texture:    return "Smoothest for pads, ambience and noisy textures.";
        case Algorithm::Repitch:    return "Tape-style: speed and pitch change together, no artifacts.";
    }
    return {};
}

std::optional<Algorithm> algorithmFromId (const juce::String& id)
{
    for (auto a : allAlgorithms)
        if (toId (a) == id)
            return a;
    return std::nullopt;
}

std::unique_ptr<TimeStretchEngine> createEngine (Algorithm a)
{
    switch (a)
    {
        case Algorithm::Percussive: return std::make_unique<TransientSliceEngine>();
        case Algorithm::Monophonic: return std::make_unique<WsolaEngine> (WsolaEngine::Config { 40.0, 14.0, "Monophonic" });
        case Algorithm::Texture:    return std::make_unique<PhaseVocoderEngine> (PhaseVocoderEngine::Config { 8192, 8, false, 1.0f, "Texture" });
        case Algorithm::Repitch:    return std::make_unique<RepitchEngine>();
        case Algorithm::Auto:
        case Algorithm::Polyphonic: break;
    }
    return std::make_unique<PhaseVocoderEngine> (PhaseVocoderEngine::Config { 4096, 4, true, 0.35f, "Polyphonic" });
}

// ---------------------------------------------------------------------------

namespace
{
    juce::AudioBuffer<float> exactLength (juce::AudioBuffer<float> b, int length)
    {
        if (b.getNumSamples() != length)
        {
            const int old = b.getNumSamples();
            b.setSize (b.getNumChannels(), length, true, true, false);
            if (length > old)
                b.clear (old, length - old);
        }
        return b;
    }
}

juce::AudioBuffer<float> PitchedEngine::process (const juce::AudioBuffer<float>& in, const Progress& progress)
{
    const auto target = (int) std::llround ((double) in.getNumSamples() * ratio);
    const double pitchFactor = std::pow (2.0, semitones / 12.0);
    if (std::abs (pitchFactor - 1.0) < 1.0e-9)
        return exactLength (stretch (in, ratio, progress), target);
    // Stretch further by the pitch factor, then resample back to the requested length.
    auto longer = stretch (in, ratio * pitchFactor, progress);
    return exactLength (dsp::resampler::toLength (longer, target), target);
}

juce::AudioBuffer<float> RepitchEngine::process (const juce::AudioBuffer<float>& in, const Progress& progress)
{
    auto out = dsp::resampler::toLength (in, std::llround ((double) in.getNumSamples() * ratio));
    if (progress.report)
        progress.report (1.0f);
    return out;
}

// ---------------------------------------------------------------------------

std::vector<int> detectOnsets (const juce::AudioBuffer<float>& in, double sr, double minGapSeconds)
{
    std::vector<int> onsets;
    const int len = in.getNumSamples();
    const int hop = juce::jmax (32, (int) std::llround (0.0025 * sr));
    const int frame = hop * 4;
    if (len < frame * 4 || in.getNumChannels() == 0)
        return onsets;

    std::vector<float> mono ((size_t) len);
    for (int i = 0; i < len; ++i)
    {
        float s = 0.0f;
        for (int ch = 0; ch < in.getNumChannels(); ++ch)
            s += in.getSample (ch, i);
        mono[(size_t) i] = s / (float) in.getNumChannels();
    }

    // High-frequency-weighted energy per frame (first difference emphasises clicks/hats).
    std::vector<double> db;
    for (int start = 0; start + frame <= len; start += hop)
    {
        double e = 0.0;
        for (int i = start + 1; i < start + frame; ++i)
        {
            const double x = mono[(size_t) i], d = x - mono[(size_t) i - 1];
            e += x * x + 4.0 * d * d;
        }
        db.push_back (10.0 * std::log10 (e / frame + 1.0e-12));
    }
    const double maxDb = *std::max_element (db.begin(), db.end());
    std::vector<double> flux (db.size(), 0.0);
    for (size_t i = 1; i < db.size(); ++i)
        flux[i] = juce::jmax (0.0, db[i] - db[i - 1]);

    const int minGap = juce::jmax (1, (int) (minGapSeconds * sr / hop));
    int last = -minGap;
    for (int i = 1; i + 1 < (int) flux.size(); ++i)
    {
        // Adaptive threshold: local mean flux + absolute 3 dB rise, and loud enough overall.
        double localMean = 0.0;
        int count = 0;
        for (int j = juce::jmax (0, i - 12); j <= juce::jmin ((int) flux.size() - 1, i + 12); ++j) { localMean += flux[(size_t) j]; ++count; }
        localMean /= juce::jmax (1, count);
        const bool peak = flux[(size_t) i] >= flux[(size_t) i - 1] && flux[(size_t) i] > flux[(size_t) i + 1];
        if (peak && flux[(size_t) i] > juce::jmax (3.0, localMean * 2.0) && db[(size_t) i] > maxDb - 45.0 && i - last >= minGap)
        {
            // Refine: first sample in this region that reaches 25% of the region's peak.
            const int from = juce::jmax (0, (i - 1) * hop), to = juce::jmin (len, (i + 4) * hop);
            float pk = 0.0f;
            for (int s = from; s < to; ++s) pk = juce::jmax (pk, std::abs (mono[(size_t) s]));
            int at = from;
            for (int s = from; s < to; ++s)
                if (std::abs (mono[(size_t) s]) >= 0.1f * pk) { at = s; break; }
            // Step back to the preceding zero crossing / quiet point (max 2 ms) so the attack is complete.
            const int limit = juce::jmax (0, at - (int) (0.002 * sr));
            while (at > limit && std::abs (mono[(size_t) at - 1]) < std::abs (mono[(size_t) at]))
                --at;
            onsets.push_back (at);
            last = i;
        }
    }
    return onsets;
}

MaterialAnalysis analyseMaterial (const juce::AudioBuffer<float>& in, double sr)
{
    MaterialAnalysis m;
    const int len = in.getNumSamples();
    if (len < 4096 || sr <= 0.0)
        return m;
    const double seconds = len / sr;
    m.onsetsPerSecond = (double) detectOnsets (in, sr).size() / seconds;

    std::vector<float> mono ((size_t) len);
    for (int i = 0; i < len; ++i)
    {
        float s = 0.0f;
        for (int ch = 0; ch < in.getNumChannels(); ++ch) s += in.getSample (ch, i);
        mono[(size_t) i] = s / (float) juce::jmax (1, in.getNumChannels());
    }

    // Periodicity: normalised autocorrelation peak (pitch range 60 Hz - 1 kHz) over loud frames.
    const int W = 2048;
    const int minLag = juce::jmax (2, (int) (sr / 1000.0)), maxLag = juce::jmin (W - 1, (int) (sr / 60.0));
    std::vector<std::pair<double, int>> frames;
    for (int start = 0; start + 2 * W <= len; start += W)
    {
        double e = 0.0;
        for (int i = 0; i < W; ++i) e += (double) mono[(size_t) (start + i)] * mono[(size_t) (start + i)];
        frames.push_back ({ e, start });
    }
    std::sort (frames.begin(), frames.end(), [] (auto& a, auto& b) { return a.first > b.first; });
    double periodicSum = 0.0;
    int used = 0;
    for (size_t f = 0; f < frames.size() && used < 16; ++f)
    {
        if (frames[f].first < 1.0e-6)
            break;
        const int start = frames[f].second;
        double best = 0.0;
        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            double ab = 0.0, aa = 1.0e-12, bb = 1.0e-12;
            for (int i = 0; i < W; i += 2)
            {
                const double x = mono[(size_t) (start + i)], y = mono[(size_t) (start + i + lag)];
                ab += x * y; aa += x * x; bb += y * y;
            }
            best = juce::jmax (best, ab / std::sqrt (aa * bb));
        }
        periodicSum += best;
        ++used;
    }
    m.periodicity = used > 0 ? periodicSum / used : 0.0;

    // Spectral flatness (geometric / arithmetic mean of the power spectrum).
    juce::dsp::FFT fft (11);
    std::vector<float> buf (4096);
    double flatSum = 0.0;
    int flatCount = 0;
    for (size_t f = 0; f < frames.size() && flatCount < 16; ++f)
    {
        if (frames[f].first < 1.0e-6)
            break;
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < W; ++i)
            buf[(size_t) i] = mono[(size_t) (frames[f].second + i)] * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / W));
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        double logSum = 0.0, sum = 0.0;
        const int lo = 4, hi = W / 2;
        for (int k = lo; k < hi; ++k)
        {
            const double p = (double) buf[(size_t) k] * buf[(size_t) k] + 1.0e-12;
            logSum += std::log (p);
            sum += p;
        }
        const int n = hi - lo;
        flatSum += std::exp (logSum / n) / (sum / n);
        ++flatCount;
    }
    m.flatness = flatCount > 0 ? flatSum / flatCount : 0.0;

    if (m.onsetsPerSecond >= 2.5)
        m.suggested = Algorithm::Percussive;
    else if (m.periodicity >= 0.85)
        m.suggested = Algorithm::Monophonic;
    else if (m.flatness >= 0.2 || (m.onsetsPerSecond < 0.5 && m.periodicity < 0.6))
        m.suggested = Algorithm::Texture;
    else
        m.suggested = Algorithm::Polyphonic;
    return m;
}
} // namespace context::stretch
