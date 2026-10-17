#pragma once

#include "AnalysisTypes.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace context
{
/** Pure, stateless analysis functions (no threads, no I/O) — easy to unit test.
    Called on the analysis worker thread only. */
namespace analysis
{
    /** Full analysis of a captured snapshot. `audio` may be mono or stereo. */
    TrackAnalysis analyze (const juce::AudioBuffer<float>& audio, double sampleRate, std::optional<double> hostBpm);

    // ---- building blocks (exposed for tests) ----
    struct KeyEstimate { int root = -1; bool minor = false; double confidence = 0.0; bool known = false; };
    /** `bass` (optional) is the pitch-class profile of the bass register: the bass mostly sits on
        the tonic, which is what tells a minor key from its relative major (same notes). */
    KeyEstimate estimateKey (const std::array<double, 12>& chroma, const std::array<double, 12>* bass = nullptr);
    std::array<double, 12> chromaFromSpectrum (const std::vector<double>& magnitude, double sampleRate, int fftSize);

    /** Pitch-class evidence summed over analysis frames (each frame counts once, so a few loud
        hits can't outvote the harmony). Treble: 100 Hz - 5 kHz with harmonic summation; bass: the
        lowest clear note (25 - 260 Hz) of each frame. */
    struct Chroma { std::array<double, 12> treble {}, bass {}; int frames = 0; };
    void accumulateFrameChroma (const float* magnitude, int bins, double sampleRate, int fftSize, Chroma& into);

    struct StereoEstimate { double sideToMid = 0.0; double correlation = 1.0; StereoWidth width = StereoWidth::Narrow; };
    StereoEstimate estimateStereo (const juce::AudioBuffer<float>& audio);

    struct OnsetEstimate { double onsetsPerSecond = 0.0; TransientDensity density = TransientDensity::Sparse;
                           std::optional<double> bpm; double tempoConfidence = 0.0; };
    OnsetEstimate estimateOnsets (const std::vector<float>& mono, double sampleRate);

    Level levelFromRatio (double ratio);
    double toDb (double linear);

    /** Minimum audio (seconds, non-silent) needed for a meaningful analysis. */
    inline constexpr double minimumSeconds = 2.0;
}
} // namespace context
