#include "TrackAnalyzer.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <numeric>

namespace context::analysis
{
namespace
{
    // Krumhansl-Kessler key profiles.
    constexpr double majorProfile[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    constexpr double minorProfile[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

    // Rough energy share of each band in a "typical" modern mix; used to label bands
    // Low / Medium / High relative to what a producer would expect. Heuristic.
    constexpr double referenceShare[NumBands] = { 0.12, 0.30, 0.16, 0.24, 0.11, 0.07 };
    constexpr double bandEdges[NumBands + 1]  = { 20.0, 80.0, 250.0, 500.0, 2000.0, 5000.0, 20000.0 };

    double pearson (const double* a, const double* b, int n)
    {
        double ma = 0, mb = 0;
        for (int i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
        ma /= n; mb /= n;
        double num = 0, da = 0, db = 0;
        for (int i = 0; i < n; ++i)
        {
            num += (a[i] - ma) * (b[i] - mb);
            da += (a[i] - ma) * (a[i] - ma);
            db += (b[i] - mb) * (b[i] - mb);
        }
        return (da > 0 && db > 0) ? num / std::sqrt (da * db) : 0.0;
    }

    int fftOrderFor (double sampleRate)
    {
        // ~5 Hz resolution or better for chroma, regardless of sample rate.
        return sampleRate > 100000.0 ? 15 : sampleRate > 50000.0 ? 14 : 13;
    }
}

double toDb (double linear) { return linear > 1.0e-12 ? 20.0 * std::log10 (linear) : -120.0; }

Level levelFromRatio (double ratio)
{
    if (ratio >= 1.6)  return Level::High;
    if (ratio <= 0.55) return Level::Low;
    return Level::Medium;
}

std::array<double, 12> chromaFromSpectrum (const std::vector<double>& mag, double sampleRate, int fftSize)
{
    std::array<double, 12> chroma {};
    const double binHz = sampleRate / fftSize;
    const int lo = juce::jmax (2, (int) (65.0 / binHz));
    const int hi = juce::jmin ((int) mag.size() - 3, (int) (4200.0 / binHz));

    double maxMag = 0;
    for (int i = lo; i <= hi; ++i)
        maxMag = juce::jmax (maxMag, mag[(size_t) i]);
    if (maxMag <= 0)
        return chroma;

    for (int i = lo; i <= hi; ++i)
    {
        const double m = mag[(size_t) i];
        // Spectral peaks only: tonal partials, not broadband noise.
        if (m < maxMag * 0.02 || m < mag[(size_t) i - 1] || m < mag[(size_t) i + 1] || m < mag[(size_t) i - 2] || m < mag[(size_t) i + 2])
            continue;
        // Parabolic interpolation for the true peak frequency.
        const double a = mag[(size_t) i - 1], b = m, c = mag[(size_t) i + 1];
        const double denom = a - 2 * b + c;
        const double offset = std::abs (denom) > 1e-12 ? 0.5 * (a - c) / denom : 0.0;
        const double freq = (i + juce::jlimit (-0.5, 0.5, offset)) * binHz;
        const double midi = 69.0 + 12.0 * std::log2 (freq / 440.0);
        const int pc = ((int) std::lround (midi) % 12 + 12) % 12;
        chroma[(size_t) pc] += m;
    }
    return chroma;
}

KeyEstimate estimateKey (const std::array<double, 12>& chroma)
{
    KeyEstimate k;
    const double total = std::accumulate (chroma.begin(), chroma.end(), 0.0);
    if (total <= 0)
        return k;

    double scores[24];
    for (int root = 0; root < 12; ++root)
    {
        double rotatedMaj[12], rotatedMin[12];
        for (int i = 0; i < 12; ++i)
        {
            rotatedMaj[(i + root) % 12] = majorProfile[i];
            rotatedMin[(i + root) % 12] = minorProfile[i];
        }
        scores[root]      = pearson (chroma.data(), rotatedMaj, 12);
        scores[12 + root] = pearson (chroma.data(), rotatedMin, 12);
    }

    int best = 0;
    for (int i = 1; i < 24; ++i)
        if (scores[i] > scores[best])
            best = i;

    k.root = best % 12;
    k.minor = best >= 12;

    // Relative major/minor share the same notes; they are not counted as competitors.
    const int relative = k.minor ? (k.root + 3) % 12 : 12 + (k.root + 9) % 12;
    double runnerUp = -1.0;
    for (int i = 0; i < 24; ++i)
        if (i != best && i != relative)
            runnerUp = juce::jmax (runnerUp, scores[i]);

    const double strength = juce::jlimit (0.0, 1.0, scores[best]);
    const double margin = juce::jlimit (0.0, 1.0, (scores[best] - runnerUp) / 0.25);
    k.confidence = juce::jlimit (0.0, 1.0, 0.6 * strength + 0.4 * margin);
    k.known = k.confidence >= 0.5 && scores[best] >= 0.5;
    return k;
}

StereoEstimate estimateStereo (const juce::AudioBuffer<float>& audio)
{
    StereoEstimate s;
    if (audio.getNumChannels() < 2 || audio.getNumSamples() == 0)
        return s;

    const float* l = audio.getReadPointer (0);
    const float* r = audio.getReadPointer (1);
    double em = 0, es = 0, ll = 0, rr = 0, lr = 0;
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const double m = 0.5 * (l[i] + r[i]), d = 0.5 * (l[i] - r[i]);
        em += m * m; es += d * d;
        ll += (double) l[i] * l[i]; rr += (double) r[i] * r[i]; lr += (double) l[i] * r[i];
    }
    s.sideToMid = (em + es) > 0 ? es / (em + es) : 0.0;
    s.correlation = (ll > 0 && rr > 0) ? lr / std::sqrt (ll * rr) : 1.0;
    s.width = s.sideToMid < 0.05 ? StereoWidth::Narrow : s.sideToMid < 0.2 ? StereoWidth::Medium : StereoWidth::Wide;
    return s;
}

OnsetEstimate estimateOnsets (const std::vector<float>& mono, double sampleRate)
{
    OnsetEstimate o;
    const int hop = 512, win = 1024;
    if ((int) mono.size() < win * 4 || sampleRate <= 0)
        return o;

    // High-frequency-weighted energy (first difference) so hats and snares register too.
    std::vector<double> energyDb;
    for (size_t start = 0; start + (size_t) win <= mono.size(); start += (size_t) hop)
    {
        double e = 0;
        for (int i = 1; i < win; ++i)
        {
            const double x = mono[start + (size_t) i], d = x - mono[start + (size_t) i - 1];
            e += x * x + 4.0 * d * d;
        }
        energyDb.push_back (10.0 * std::log10 (e / win + 1e-12));
    }

    const double maxDb = *std::max_element (energyDb.begin(), energyDb.end());
    std::vector<double> flux (energyDb.size(), 0.0);
    for (size_t i = 1; i < energyDb.size(); ++i)
        flux[i] = juce::jmax (0.0, energyDb[i] - energyDb[i - 1]);

    const int minGap = juce::jmax (1, (int) (0.06 * sampleRate / hop));
    int onsets = 0, lastOnset = -minGap;
    for (int i = 1; i + 1 < (int) flux.size(); ++i)
    {
        const bool peak = flux[(size_t) i] >= flux[(size_t) i - 1] && flux[(size_t) i] > flux[(size_t) i + 1];
        if (peak && flux[(size_t) i] > 3.0 && energyDb[(size_t) i] > maxDb - 40.0 && i - lastOnset >= minGap)
        {
            ++onsets;
            lastOnset = i;
        }
    }

    const double seconds = mono.size() / sampleRate;
    o.onsetsPerSecond = onsets / seconds;
    o.density = o.onsetsPerSecond < 1.5 ? TransientDensity::Sparse : o.onsetsPerSecond < 4.0 ? TransientDensity::Moderate : TransientDensity::Dense;

    // Tempo: autocorrelation of the onset-strength envelope over 70-180 BPM.
    const double framesPerSecond = sampleRate / hop;
    const int minLag = (int) std::floor (framesPerSecond * 60.0 / 180.0);
    const int maxLag = (int) std::ceil (framesPerSecond * 60.0 / 70.0);
    if ((int) flux.size() > maxLag * 3 && onsets >= 4)
    {
        const double mean = std::accumulate (flux.begin(), flux.end(), 0.0) / (double) flux.size();
        std::vector<double> env (flux.size());
        for (size_t i = 0; i < flux.size(); ++i) env[i] = flux[i] - mean;

        auto ac = [&] (int lag)
        {
            double sum = 0;
            for (size_t i = (size_t) lag; i < env.size(); ++i) sum += env[i] * env[i - (size_t) lag];
            return sum / (double) (env.size() - (size_t) lag);
        };
        const double zero = ac (0);
        int bestLag = 0;
        double bestVal = 0;
        std::vector<double> vals ((size_t) maxLag + 2, 0.0);
        for (int lag = minLag - 1; lag <= maxLag + 1; ++lag)
            vals[(size_t) lag] = ac (lag);
        for (int lag = minLag; lag <= maxLag; ++lag)
            if (vals[(size_t) lag] > bestVal) { bestVal = vals[(size_t) lag]; bestLag = lag; }

        if (bestLag > 0 && zero > 0)
        {
            const double a = vals[(size_t) bestLag - 1], b = vals[(size_t) bestLag], c = vals[(size_t) bestLag + 1];
            const double denom = a - 2 * b + c;
            const double refined = bestLag + (std::abs (denom) > 1e-12 ? juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / denom) : 0.0);
            o.tempoConfidence = juce::jlimit (0.0, 1.0, bestVal / zero);
            if (o.tempoConfidence > 0.15)
                o.bpm = 60.0 * framesPerSecond / refined;
        }
    }
    return o;
}

TrackAnalysis analyze (const juce::AudioBuffer<float>& audio, double sampleRate, std::optional<double> hostBpm)
{
    TrackAnalysis t;
    t.hostBpm = (hostBpm && *hostBpm >= 20.0 && *hostBpm <= 999.0) ? hostBpm : std::nullopt;
    t.sampleRate = sampleRate;
    t.channels = audio.getNumChannels();
    const int n = audio.getNumSamples();
    if (n <= 0 || sampleRate <= 0 || audio.getNumChannels() == 0)
        return t;

    t.secondsAnalyzed = n / sampleRate;

    // Mono downmix + level statistics
    std::vector<float> mono ((size_t) n);
    const int chans = juce::jmin (2, audio.getNumChannels());
    double sumSq = 0, peak = 0;
    for (int i = 0; i < n; ++i)
    {
        double m = 0;
        for (int ch = 0; ch < chans; ++ch)
        {
            const double x = audio.getSample (ch, i);
            m += x;
            peak = juce::jmax (peak, std::abs (x));
        }
        m /= chans;
        mono[(size_t) i] = (float) m;
        sumSq += m * m;
    }
    t.rmsDb = toDb (std::sqrt (sumSq / n));
    t.peakDb = toDb (peak);
    t.approxLoudnessDb = t.rmsDb - 0.7;   // crude LUFS-like offset; labelled "approximate"

    if (t.rmsDb < -70.0)
        return t;   // effectively silent: valid stays false

    // Average spectrum
    const int order = fftOrderFor (sampleRate);
    const int fftSize = 1 << order;
    juce::dsp::FFT fft (order);
    juce::dsp::WindowingFunction<float> window ((size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false);
    std::vector<float> frame ((size_t) fftSize * 2);
    std::vector<double> power ((size_t) fftSize / 2 + 1, 0.0), magnitude ((size_t) fftSize / 2 + 1, 0.0);
    int frames = 0;
    const int hop = fftSize / 2;
    for (int start = 0; start + fftSize <= n || (frames == 0 && start == 0); start += hop)
    {
        std::fill (frame.begin(), frame.end(), 0.0f);
        const int len = juce::jmin (fftSize, n - start);
        std::copy (mono.begin() + start, mono.begin() + start + len, frame.begin());
        window.multiplyWithWindowingTable (frame.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (frame.data(), true);
        for (size_t b = 0; b < power.size(); ++b)
        {
            const double m = frame[b];
            magnitude[b] += m;
            power[b] += m * m;
        }
        ++frames;
        if (start + fftSize > n)
            break;
    }

    const double binHz = sampleRate / fftSize;
    double total = 0, weighted = 0, magTotal = 0;
    std::array<double, NumBands> bandEnergy {};
    for (size_t b = 1; b < power.size(); ++b)
    {
        const double f = b * binHz;
        if (f < 20.0 || f > 20000.0)
            continue;
        total += power[b];
        weighted += f * magnitude[b];
        magTotal += magnitude[b];
        for (int band = 0; band < NumBands; ++band)
            if (f >= bandEdges[band] && f < bandEdges[band + 1])
                bandEnergy[(size_t) band] += power[b];
    }
    if (total <= 0)
        return t;

    t.spectralCentroidHz = magTotal > 0 ? weighted / magTotal : 0.0;
    double cumulative = 0;
    for (size_t b = 1; b < power.size(); ++b)
    {
        const double f = b * binHz;
        if (f < 20.0) continue;
        cumulative += power[b];
        if (cumulative >= 0.85 * total) { t.spectralRolloffHz = f; break; }
    }

    for (int band = 0; band < NumBands; ++band)
    {
        t.bandShare[(size_t) band] = bandEnergy[(size_t) band] / total;
        t.bandLevel[(size_t) band] = levelFromRatio (t.bandShare[(size_t) band] / referenceShare[band]);
    }
    t.lowEnd = levelFromRatio ((t.bandShare[Sub] + t.bandShare[Bass]) / (referenceShare[Sub] + referenceShare[Bass]));
    t.mids   = levelFromRatio ((t.bandShare[LowMid] + t.bandShare[Mid]) / (referenceShare[LowMid] + referenceShare[Mid]));
    t.highs  = levelFromRatio ((t.bandShare[UpperMid] + t.bandShare[High]) / (referenceShare[UpperMid] + referenceShare[High]));
    t.tonal  = t.spectralCentroidHz < 1000.0 ? TonalBalance::Dark : t.spectralCentroidHz > 3000.0 ? TonalBalance::Bright : TonalBalance::Balanced;

    // Key
    for (auto& m : magnitude) m /= frames;
    const auto key = estimateKey (chromaFromSpectrum (magnitude, sampleRate, fftSize));
    t.keyKnown = key.known;
    t.keyRoot = key.root;
    t.minor = key.minor;
    t.keyConfidence = key.confidence;

    // Stereo + transients + tempo estimate
    const auto stereo = estimateStereo (audio);
    t.sideToMidRatio = stereo.sideToMid;
    t.correlation = stereo.correlation;
    t.stereo = stereo.width;

    const auto onsets = estimateOnsets (mono, sampleRate);
    t.onsetsPerSecond = onsets.onsetsPerSecond;
    t.transients = onsets.density;
    t.estimatedBpm = onsets.bpm;
    t.tempoConfidence = onsets.tempoConfidence;

    t.valid = true;
    return t;
}
} // namespace context::analysis
