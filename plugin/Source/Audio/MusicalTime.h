#pragma once

#include <juce_core/juce_core.h>
#include <optional>

namespace context::musical
{
/** Musical time conventions used everywhere in Context:
      - BPM is quarter notes per minute (what every DAW reports).
      - A bar holds numerator * (4 / denominator) quarter notes (4/4 -> 4, 6/8 -> 3, 7/8 -> 3.5).
      - Durations are converted to samples ONCE, with a single rounding step, from the
        bar count. Nothing ever accumulates per-bar or per-block, so lengths never drift. */
struct TimeSignature
{
    int numerator = 4;
    int denominator = 4;

    bool isValid() const noexcept
    {
        return numerator >= 1 && numerator <= 32
            && (denominator == 1 || denominator == 2 || denominator == 4 || denominator == 8 || denominator == 16 || denominator == 32);
    }
    bool operator== (const TimeSignature&) const = default;
    juce::String toString() const { return juce::String (numerator) + "/" + juce::String (denominator); }
};

/** A tempo plus metre. */
struct Tempo
{
    double bpm = 120.0;
    TimeSignature signature;

    bool isValid() const noexcept;
    bool operator== (const Tempo&) const = default;
};

inline constexpr double minBpm = 20.0;
inline constexpr double maxBpm = 999.0;
inline bool isValidBpm (double bpm) noexcept { return std::isfinite (bpm) && bpm >= minBpm && bpm <= maxBpm; }
inline bool Tempo::isValid() const noexcept { return isValidBpm (bpm) && signature.isValid(); }

double quarterNotesPerBar (TimeSignature) noexcept;

/** seconds = bars * quarterNotesPerBar * 60 / bpm */
double secondsForBars (double bars, const Tempo&) noexcept;

/** Exact sample count for `bars` bars: llround (bars * qpb * 60 * sampleRate / bpm). */
juce::int64 samplesForBars (double bars, const Tempo&, double sampleRate) noexcept;

/** Fractional samples in one bar (for drawing grids; never used to build lengths). */
double samplesPerBar (const Tempo&, double sampleRate) noexcept;

double barsForSamples (juce::int64 samples, const Tempo&, double sampleRate) noexcept;
double barsForSeconds (double seconds, const Tempo&) noexcept;

/** PPQ position of the first bar line at or after `ppq`.
    If the host reports the position of the last bar start, that is used (it is
    correct even after time-signature changes); otherwise bars are assumed to start
    at PPQ 0 (true for a constant metre). Positions within `epsilon` quarter notes
    after a bar line count as being on it. */
double nextBarStartPpq (double ppq, TimeSignature, std::optional<double> lastBarStartPpq, double epsilon = 1.0e-6) noexcept;

/** Tempo implied by a loop of `seconds` that spans `bars` bars. */
double bpmForLoop (double seconds, double bars, TimeSignature) noexcept;

/** Bar counts offered by the length selector. */
inline constexpr int lengthChoices[] = { 2, 4, 6, 8, 16 };
inline bool isValidLengthBars (int bars) noexcept
{
    for (auto b : lengthChoices)
        if (b == bars)
            return true;
    return false;
}

/** Number of bars the context recorder captures. */
inline constexpr int contextBars = 8;

/** Parses "130bpm", "130 BPM", "_130_" style hints from a file name. */
std::optional<double> bpmFromFileName (const juce::String& name);
} // namespace context::musical
