#include "StretchEngines.h"
#include <juce_dsp/juce_dsp.h>
#include <complex>
#include <vector>

namespace context::stretch
{
PhaseVocoderEngine::PhaseVocoderEngine (Config c) : cfg (std::move (c)) {}

namespace
{
    inline double princarg (double p) noexcept
    {
        const double twoPi = juce::MathConstants<double>::twoPi;
        p = std::fmod (p + juce::MathConstants<double>::pi, twoPi);
        if (p < 0.0) p += twoPi;
        return p - juce::MathConstants<double>::pi;
    }
}

juce::AudioBuffer<float> PhaseVocoderEngine::stretch (const juce::AudioBuffer<float>& in, double alpha, const Progress& progress)
{
    const int chans = juce::jmax (1, in.getNumChannels());
    const int inLen = in.getNumSamples();
    const auto outLen = (int) std::llround ((double) inLen * alpha);
    juce::AudioBuffer<float> out (chans, juce::jmax (0, outLen));
    out.clear();
    if (inLen <= 0 || outLen <= 0)
        return out;

    // Keep the window length roughly constant in time across sample rates.
    int N = cfg.fftSize;
    while (sampleRate > 0.0 && N * 48000.0 / sampleRate < cfg.fftSize * 0.75 && N < 32768)
        N *= 2;
    const int order = juce::roundToInt (std::log2 ((double) N));
    const int bins = N / 2 + 1;
    const int Hs = N / juce::jmax (2, cfg.overlap);
    const int pad = N;

    juce::dsp::FFT fft (order);
    std::vector<float> window ((size_t) N);
    for (int n = 0; n < N; ++n)
        window[(size_t) n] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * n / N));

    // Calibrate the inverse transform's scaling once (engines differ).
    std::vector<float> work ((size_t) N * 2, 0.0f);
    work[1] = 1.0f;
    fft.performRealOnlyForwardTransform (work.data(), true);
    fft.performRealOnlyInverseTransform (work.data());
    const float inverseScale = std::abs (work[1]) > 1.0e-9f ? 1.0f / work[1] : 1.0f;

    std::vector<std::vector<float>> spectra ((size_t) chans + 1, std::vector<float> ((size_t) N * 2));
    std::vector<double> prevPhase ((size_t) bins, 0.0), synthPhase ((size_t) bins, 0.0), prevMag ((size_t) bins, 0.0);
    std::vector<double> mag ((size_t) bins), phase ((size_t) bins);
    std::vector<int> peaks;
    peaks.reserve ((size_t) bins);

    juce::AudioBuffer<float> acc (chans, outLen + 2 * pad);
    acc.clear();
    std::vector<float> norm ((size_t) outLen + 2 * (size_t) pad, 0.0f);

    const int numFrames = outLen / Hs + 3;
    int prevA = 0;
    for (int m = 0; m < numFrames; ++m)
    {
        if ((m & 15) == 0)
        {
            if (progress.cancelled())
                return out;
            if (progress.report)
                progress.report ((float) m / (float) numFrames);
        }

        const double centre = (double) m * Hs / alpha;
        const int a = (int) std::llround (centre);
        const int da = m == 0 ? juce::jmax (1, (int) std::llround (Hs / alpha)) : a - prevA;
        prevA = a;
        const int start = a - N / 2;

        // Analysis: each channel, plus the mid signal that drives phase propagation.
        auto& mid = spectra[(size_t) chans];
        std::fill (mid.begin(), mid.end(), 0.0f);
        for (int ch = 0; ch < chans; ++ch)
        {
            auto& s = spectra[(size_t) ch];
            std::fill (s.begin(), s.end(), 0.0f);
            const float* src = in.getReadPointer (ch);
            for (int n = 0; n < N; ++n)
            {
                const int idx = start + n;
                const float x = (idx >= 0 && idx < inLen) ? src[idx] * window[(size_t) n] : 0.0f;
                s[(size_t) n] = x;
                mid[(size_t) n] += x / (float) chans;
            }
            fft.performRealOnlyForwardTransform (s.data(), true);
        }
        fft.performRealOnlyForwardTransform (mid.data(), true);

        double fluxUp = 0.0, total = 1.0e-12;
        for (int k = 0; k < bins; ++k)
        {
            const double re = mid[(size_t) (2 * k)], im = mid[(size_t) (2 * k + 1)];
            mag[(size_t) k] = std::sqrt (re * re + im * im);
            phase[(size_t) k] = std::atan2 (im, re);
            fluxUp += juce::jmax (0.0, mag[(size_t) k] - prevMag[(size_t) k]);
            total += mag[(size_t) k];
        }
        const bool transient = cfg.transientReset && m > 0 && fluxUp / total > cfg.transientThreshold;

        if (m == 0 || transient)
        {
            // Phase reset: take the analysis phases as they are (keeps attacks sharp).
            for (int k = 0; k < bins; ++k)
                synthPhase[(size_t) k] = phase[(size_t) k];
        }
        else
        {
            peaks.clear();
            for (int k = 2; k < bins - 2; ++k)
                if (mag[(size_t) k] > mag[(size_t) k - 1] && mag[(size_t) k] >= mag[(size_t) k + 1]
                    && mag[(size_t) k] > mag[(size_t) k - 2] && mag[(size_t) k] >= mag[(size_t) k + 2])
                    peaks.push_back (k);

            const double twoPiOverN = juce::MathConstants<double>::twoPi / N;
            std::vector<double> newPhase (synthPhase);
            for (int p : peaks)
            {
                const double omega = twoPiOverN * p;
                const double dphi = princarg (phase[(size_t) p] - prevPhase[(size_t) p] - omega * da);
                const double instFreq = omega + dphi / juce::jmax (1, da);
                newPhase[(size_t) p] = synthPhase[(size_t) p] + instFreq * Hs;
            }
            if (peaks.empty())
            {
                for (int k = 0; k < bins; ++k)
                {
                    const double omega = twoPiOverN * k;
                    const double dphi = princarg (phase[(size_t) k] - prevPhase[(size_t) k] - omega * da);
                    newPhase[(size_t) k] = synthPhase[(size_t) k] + (omega + dphi / juce::jmax (1, da)) * Hs;
                }
            }
            else
            {
                // Identity phase locking: bins follow the phase of their region's peak.
                size_t pi = 0;
                for (int k = 0; k < bins; ++k)
                {
                    while (pi + 1 < peaks.size() && std::abs (peaks[pi + 1] - k) < std::abs (peaks[pi] - k))
                        ++pi;
                    const int p = peaks[pi];
                    if (k != p)
                        newPhase[(size_t) k] = newPhase[(size_t) p] + (phase[(size_t) k] - phase[(size_t) p]);
                }
            }
            synthPhase.swap (newPhase);
        }
        prevPhase = phase;
        prevMag = mag;

        // Synthesis per channel, keeping each channel's phase offset from the mid signal.
        const int s0 = m * Hs - N / 2 + pad;
        for (int ch = 0; ch < chans; ++ch)
        {
            auto& s = spectra[(size_t) ch];
            for (int k = 0; k < bins; ++k)
            {
                const double re = s[(size_t) (2 * k)], im = s[(size_t) (2 * k + 1)];
                const double amp = std::sqrt (re * re + im * im);
                const double ph = synthPhase[(size_t) k] + (std::atan2 (im, re) - phase[(size_t) k]);
                s[(size_t) (2 * k)] = (float) (amp * std::cos (ph));
                s[(size_t) (2 * k + 1)] = (float) (amp * std::sin (ph));
            }
            fft.performRealOnlyInverseTransform (s.data());
            float* dst = acc.getWritePointer (ch);
            for (int n = 0; n < N; ++n)
            {
                const int o = s0 + n;
                if (o >= 0 && o < acc.getNumSamples())
                    dst[o] += s[(size_t) n] * inverseScale * window[(size_t) n];
            }
        }
        for (int n = 0; n < N; ++n)
        {
            const int o = s0 + n;
            if (o >= 0 && o < (int) norm.size())
                norm[(size_t) o] += window[(size_t) n] * window[(size_t) n];
        }
    }

    // Normalise by the summed squared window (exact, including the edges).
    const float steady = 1.5f * (float) N / (4.0f * (float) Hs) ;   // Hann^2 overlap sum
    for (int ch = 0; ch < chans; ++ch)
    {
        const float* src = acc.getReadPointer (ch, pad);
        float* dst = out.getWritePointer (ch);
        for (int i = 0; i < outLen; ++i)
            dst[i] = src[i] / juce::jmax (norm[(size_t) (i + pad)], steady * 0.1f);
    }
    if (progress.report)
        progress.report (1.0f);
    return out;
}
} // namespace context::stretch
