#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <optional>

namespace context
{
enum class Level { Low, Medium, High };
inline juce::String toString (Level l) { return l == Level::Low ? "Low" : l == Level::Medium ? "Medium" : "High"; }

enum class StereoWidth { Narrow, Medium, Wide };
inline juce::String toString (StereoWidth w) { return w == StereoWidth::Narrow ? "Narrow" : w == StereoWidth::Medium ? "Medium" : "Wide"; }

enum class TransientDensity { Sparse, Moderate, Dense };
inline juce::String toString (TransientDensity d) { return d == TransientDensity::Sparse ? "Sparse" : d == TransientDensity::Moderate ? "Moderate" : "Dense"; }

enum class TonalBalance { Dark, Balanced, Bright };
inline juce::String toString (TonalBalance t) { return t == TonalBalance::Dark ? "Dark" : t == TonalBalance::Balanced ? "Balanced" : "Bright"; }

/** Bands: Sub 20-80, Bass 80-250, LowMid 250-500, Mid 500-2k, UpperMid 2k-5k, High 5k-20k. */
enum Band { Sub = 0, Bass, LowMid, Mid, UpperMid, High, NumBands };
inline const char* bandName (int b)
{
    static const char* names[] = { "Sub", "Bass", "Low Mid", "Mid", "Upper Mid", "High" };
    return (b >= 0 && b < NumBands) ? names[b] : "?";
}

/** Result of analysing the captured track audio. Everything here is derived
    locally; none of it contains audio. */
struct TrackAnalysis
{
    bool valid = false;
    double secondsAnalyzed = 0.0;
    double sampleRate = 0.0;
    int channels = 0;

    std::optional<double> hostBpm;           // from the DAW play-head (authoritative)
    std::optional<double> estimatedBpm;      // from onset autocorrelation (used only without host BPM)
    double tempoConfidence = 0.0;

    // Key: keyKnown=false means "Unknown" (confidence below threshold)
    bool keyKnown = false;
    int keyRoot = -1;                        // 0 = C ... 11 = B
    bool minor = false;
    double keyConfidence = 0.0;              // 0..1

    double rmsDb = -120.0, peakDb = -120.0, approxLoudnessDb = -120.0;
    double spectralCentroidHz = 0.0, spectralRolloffHz = 0.0;

    std::array<double, NumBands> bandShare {};   // fraction of total energy per band
    std::array<Level, NumBands> bandLevel {};
    Level lowEnd = Level::Medium, mids = Level::Medium, highs = Level::Medium;

    double sideToMidRatio = 0.0;             // 0 = mono
    double correlation = 1.0;                // L/R correlation
    StereoWidth stereo = StereoWidth::Narrow;

    double onsetsPerSecond = 0.0;
    TransientDensity transients = TransientDensity::Sparse;
    TonalBalance tonal = TonalBalance::Balanced;

    std::optional<double> bestBpm() const { return hostBpm.has_value() ? hostBpm : estimatedBpm; }

    juce::String keyName() const;            // "F#" or "" when unknown
    juce::String modeName() const;           // "Minor"/"Major" or "" when unknown
    juce::String keyDisplay() const;         // "F# Minor" / "Unknown"

    juce::var toVar() const;                 // compact metadata (no audio)
};

inline juce::String pitchClassName (int pc)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return (pc >= 0 && pc < 12) ? names[pc] : "";
}

inline juce::String TrackAnalysis::keyName() const   { return keyKnown ? pitchClassName (keyRoot) : juce::String(); }
inline juce::String TrackAnalysis::modeName() const  { return keyKnown ? juce::String (minor ? "Minor" : "Major") : juce::String(); }
inline juce::String TrackAnalysis::keyDisplay() const { return keyKnown ? keyName() + " " + modeName() : juce::String ("Unknown"); }

inline juce::var TrackAnalysis::toVar() const
{
    auto* o = new juce::DynamicObject();
    if (auto b = bestBpm())
    {
        o->setProperty ("bpm", juce::roundToInt (*b * 10.0) / 10.0);
        o->setProperty ("bpmSource", hostBpm.has_value() ? "host" : "estimated");
    }
    if (keyKnown)
    {
        o->setProperty ("key", keyName());
        o->setProperty ("mode", modeName().toLowerCase());
    }
    o->setProperty ("keyConfidence", juce::roundToInt (keyConfidence * 100.0) / 100.0);
    o->setProperty ("lowEnd", toString (lowEnd));
    o->setProperty ("mids", toString (mids));
    o->setProperty ("highs", toString (highs));
    o->setProperty ("stereoWidth", toString (stereo));
    o->setProperty ("transientDensity", toString (transients));
    o->setProperty ("tonalBalance", toString (tonal));
    o->setProperty ("loudnessDb", juce::roundToInt (approxLoudnessDb * 10.0) / 10.0);
    o->setProperty ("spectralCentroidHz", juce::roundToInt (spectralCentroidHz));
    return juce::var (o);
}
} // namespace context
