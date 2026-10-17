#include "StretchEngines.h"
#include "Audio/Resampler.h"
#include <vector>

namespace context::stretch
{
WsolaEngine::WsolaEngine (Config c) : cfg (std::move (c)) {}

juce::AudioBuffer<float> WsolaEngine::stretch (const juce::AudioBuffer<float>& in, double timeRatio, const Progress& progress)
{
    return wsola (in, std::llround ((double) in.getNumSamples() * timeRatio), sampleRate, cfg, &progress);
}

juce::AudioBuffer<float> WsolaEngine::wsola (const juce::AudioBuffer<float>& in, juce::int64 outLength, double sr, const Config& cfg, const Progress* progress)
{
    const int chans = juce::jmax (1, in.getNumChannels());
    const int inLen = in.getNumSamples();
    const int outLen = (int) juce::jmax<juce::int64> (0, outLength);
    juce::AudioBuffer<float> out (chans, outLen);
    out.clear();
    if (inLen <= 0 || outLen <= 0)
        return out;

    int N = juce::jmax (64, (int) std::llround (cfg.frameMs * 0.001 * sr));
    N += N & 1;
    const int Hs = N / 2;
    const int tol = juce::jmax (1, (int) std::llround (cfg.toleranceMs * 0.001 * sr));
    const double alpha = (double) outLen / (double) inLen;

    std::vector<float> window ((size_t) N);
    for (int n = 0; n < N; ++n)
        window[(size_t) n] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * n / N));   // periodic Hann: sums to 1 at 50%

    // Mono guide signal for the similarity search (+ a 4x decimated copy for a fast coarse pass).
    std::vector<float> mono ((size_t) inLen);
    for (int i = 0; i < inLen; ++i)
    {
        float s = 0.0f;
        for (int ch = 0; ch < in.getNumChannels(); ++ch)
            s += in.getSample (ch, i);
        mono[(size_t) i] = s / (float) chans;
    }
    constexpr int dec = 4;
    std::vector<float> monoD ((size_t) (inLen / dec + 1), 0.0f);
    for (int i = 0; i + dec <= inLen; i += dec)
        monoD[(size_t) (i / dec)] = 0.25f * (mono[(size_t) i] + mono[(size_t) i + 1] + mono[(size_t) i + 2] + mono[(size_t) i + 3]);

    auto sample = [&] (const std::vector<float>& v, int i) { return (i >= 0 && i < (int) v.size()) ? v[(size_t) i] : 0.0f; };
    auto correlation = [&] (const std::vector<float>& v, int a, int b, int len, int step)
    {
        double ab = 0.0, aa = 1.0e-12, bb = 1.0e-12;
        for (int n = 0; n < len; n += step)
        {
            const double x = sample (v, a + n), y = sample (v, b + n);
            ab += x * y;
            aa += x * x;
            bb += y * y;
        }
        return ab / std::sqrt (aa * bb);
    };

    juce::AudioBuffer<float> acc (chans, outLen + N);
    acc.clear();
    std::vector<float> norm ((size_t) outLen + (size_t) N, 0.0f);

    int prevPos = 0;
    const int numFrames = (outLen + N / 2) / Hs + 1;
    for (int m = 0; m < numFrames; ++m)
    {
        if (progress != nullptr && (m & 63) == 0)
        {
            if (progress->cancelled())
                return out;
            if (progress->report)
                progress->report ((float) m / (float) numFrames);
        }

        const int outStart = m * Hs - N / 2;
        const int target = (int) std::llround ((double) m * Hs / alpha) - N / 2;
        int pos = target;
        if (m > 0)
        {
            // Choose the candidate whose start best continues the previous frame's waveform.
            const int natural = prevPos + Hs;
            int best = 0;
            double bestCorr = -2.0;
            for (int d = -tol / dec; d <= tol / dec; ++d)
            {
                const double c = correlation (monoD, (target / dec) + d, natural / dec, Hs / dec, 1);
                if (c > bestCorr) { bestCorr = c; best = d * dec; }
            }
            int refined = best;
            bestCorr = -2.0;
            for (int d = best - dec; d <= best + dec; ++d)
            {
                if (std::abs (d) > tol)
                    continue;
                const double c = correlation (mono, target + d, natural, Hs, 2);
                if (c > bestCorr) { bestCorr = c; refined = d; }
            }
            pos = target + refined;
        }

        for (int ch = 0; ch < chans; ++ch)
        {
            const float* src = in.getReadPointer (juce::jmin (ch, in.getNumChannels() - 1));
            float* dst = acc.getWritePointer (ch);
            for (int n = 0; n < N; ++n)
            {
                const int o = outStart + n + N / 2;   // acc is offset by N/2
                const int s = pos + n;
                if (o >= 0 && o < acc.getNumSamples() && s >= 0 && s < inLen)
                    dst[o] += src[s] * window[(size_t) n];
            }
        }
        for (int n = 0; n < N; ++n)
        {
            const int o = outStart + n + N / 2;
            if (o >= 0 && o < (int) norm.size())
                norm[(size_t) o] += window[(size_t) n];
        }
        prevPos = pos;
    }

    for (int ch = 0; ch < chans; ++ch)
    {
        const float* src = acc.getReadPointer (ch, N / 2);
        float* dst = out.getWritePointer (ch);
        for (int i = 0; i < outLen; ++i)
            dst[i] = src[i] / juce::jmax (norm[(size_t) (i + N / 2)], 0.05f);
    }
    return out;
}

// ---------------------------------------------------------------------------

juce::AudioBuffer<float> TransientSliceEngine::stretch (const juce::AudioBuffer<float>& in, double alpha, const Progress& progress)
{
    const int chans = juce::jmax (1, in.getNumChannels());
    const int inLen = in.getNumSamples();
    const auto outLen = (int) std::llround ((double) inLen * alpha);
    juce::AudioBuffer<float> out (chans, juce::jmax (0, outLen));
    out.clear();
    if (inLen <= 0 || outLen <= 0)
        return out;

    auto onsets = detectOnsets (in, sampleRate, 0.045);
    std::vector<int> bounds { 0 };
    for (int o : onsets)
        if (o > bounds.back() + 32 && o < inLen - 32)
            bounds.push_back (o);
    bounds.push_back (inLen);

    const int attack = juce::jmax (16, (int) std::llround (0.012 * sampleRate));
    const int fade = juce::jmax (8, (int) std::llround (0.003 * sampleRate));
    const WsolaEngine::Config tailCfg { 15.0, 6.0, "tail" };

    for (size_t k = 0; k + 1 < bounds.size(); ++k)
    {
        if (progress.cancelled())
            return out;
        if (progress.report)
            progress.report ((float) k / (float) bounds.size());

        const int s = bounds[k], e = bounds[k + 1];
        const int os = (int) std::llround (s * alpha);
        const int oe = k + 2 == bounds.size() ? outLen : (int) std::llround (e * alpha);
        const int slot = juce::jmin (oe, outLen) - os;
        const int seg = e - s;
        if (slot <= 0 || seg <= 0)
            continue;

        // 1. The attack is copied sample for sample: transients are never smeared.
        const int a = juce::jmin (attack, seg, slot);
        for (int ch = 0; ch < chans; ++ch)
            out.copyFrom (ch, os, in, juce::jmin (ch, in.getNumChannels() - 1), s, a);

        // 2. Whatever follows the attack is time-scaled to fill the rest of the slot. The last
        //    1.5 ms before the next onset is left out: detected onsets can be a few samples
        //    late, and the start of the next hit must never be smeared into this tail.
        const int guard = k + 2 < bounds.size() ? juce::jmin ((int) (0.0015 * sampleRate), (seg - a) / 4) : 0;
        const int remSrc = seg - a - guard, remSlot = slot - a;
        if (remSlot > 0 && remSrc > 0)
        {
            juce::AudioBuffer<float> rest (in.getNumChannels(), remSrc);
            for (int ch = 0; ch < in.getNumChannels(); ++ch)
                rest.copyFrom (ch, 0, in, ch, s + a, remSrc);
            const auto scaled = (remSlot == remSrc) ? rest : WsolaEngine::wsola (rest, remSlot, sampleRate, tailCfg);
            for (int ch = 0; ch < chans; ++ch)
                out.copyFrom (ch, os + a, scaled, juce::jmin (ch, scaled.getNumChannels() - 1), 0, juce::jmin (remSlot, scaled.getNumSamples()));
        }

        // 3. Short fade at the end of each slot so the next hit starts cleanly.
        if (k + 2 < bounds.size())
        {
            const int f = juce::jmin (fade, slot);
            for (int ch = 0; ch < chans; ++ch)
            {
                float* d = out.getWritePointer (ch, os + slot - f);
                for (int i = 0; i < f; ++i)
                    d[i] *= 1.0f - (float) (i + 1) / (float) f;
            }
        }
    }
    if (progress.report)
        progress.report (1.0f);
    return out;
}
} // namespace context::stretch
