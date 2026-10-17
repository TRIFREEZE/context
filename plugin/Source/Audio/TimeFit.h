#pragma once

#include "AudioAsset.h"
#include "MusicalTime.h"
#include "TimeStretch/TimeStretchEngine.h"

namespace context
{
/** How a source is made to occupy exactly N bars.
      Auto     choose from the material and the size of the mismatch
      Stretch  time-stretch the whole source to the target length ("Fit")
      Trim     cut to the target length (shorter sources are padded with silence)
      Loop     repeat to fill; sources close to a whole number of bars are first
               stretched to exactly that many bars so every repeat lands on the grid
      Repitch  resample (tempo and pitch together) */
enum class FitMode { Auto, Stretch, Trim, Loop, Repitch };

juce::String toId (FitMode);
juce::String toLabel (FitMode);
juce::String tooltipFor (FitMode);
std::optional<FitMode> fitModeFromId (const juce::String&);
inline constexpr FitMode allFitModes[] = { FitMode::Auto, FitMode::Stretch, FitMode::Trim, FitMode::Loop, FitMode::Repitch };

struct FitSettings
{
    FitMode mode = FitMode::Auto;
    stretch::Algorithm algorithm = stretch::Algorithm::Auto;
    double pitchSemitones = 0.0;
    /** Remove dead space (silence before the first sound, dead air after the last) from
        generated sounds before fitting, so they start on the bar line. Never applied to
        imported samples or to audio generated on the context. */
    bool tighten = true;

    bool operator== (const FitSettings&) const = default;
    juce::var toVar() const;
    static FitSettings fromVar (const juce::var&);
};

/** What the renderer did, in numbers the UI can show honestly. */
struct FitReport
{
    FitMode modeUsed = FitMode::Trim;
    stretch::Algorithm algorithmUsed = stretch::Algorithm::Polyphonic;   // when a stretch engine ran
    bool engineUsed = false;
    double sourceSeconds = 0.0;
    double sourceBars = 0.0;            // source length measured in bars of the target tempo
    int targetBars = 0;
    juce::int64 targetSamples = 0;
    double outputRate = 0.0;
    double stretchPercent = 0.0;        // +6.1 = 6.1% longer
    int loopRepeats = 1;
    std::optional<double> sourceBpm;    // known or inferred tempo of the source
    juce::String sourceBpmOrigin;       // "generator", "file name", "length", "analysis"
    double pitchSemitones = 0.0;        // net pitch change of the result
    double trimmedStartSeconds = 0.0;   // dead space removed before the first sound
    double trimmedEndSeconds = 0.0;     // ... and after the last
    bool aligned = false;               // source was generated on the context (kept in place)
    int alignedStartBar = 0;            // aligned and longer than the target: first kept bar (0 = from the start)
};

struct FitRequest
{
    AudioAsset::Ptr source;
    musical::Tempo tempo;
    int bars = 8;
    double outputRate = 48000.0;
    FitSettings settings;
    /** Cached Auto analysis for this source (computed if missing). */
    std::optional<stretch::MaterialAnalysis> material;
};

struct FitResult
{
    std::shared_ptr<const juce::AudioBuffer<float>> audio;   // stereo, exactly report.targetSamples
    FitReport report;
    juce::String error;
    bool ok() const { return audio != nullptr; }
};

namespace timefit
{
    /** Offline render (worker thread). Never touches the source asset. */
    FitResult render (const FitRequest&, const stretch::TimeStretchEngine::Progress& = {});

    /** Tempo of the source if it can be known or confidently inferred. */
    std::optional<double> inferSourceBpm (const AudioAsset&, const musical::Tempo& project, juce::String& origin);

    /** The decision Auto makes (exposed for tests and the UI preview). */
    FitMode chooseMode (double sourceBars, int targetBars, bool shortSound);

    /** Divisor of targetBars the source is within tolerance of (for grid-locked loops), or 0. */
    int loopUnitBars (double sourceBars, int targetBars, double tolerance = 0.08);

    /** Where the sound actually is: [start, end) in samples, ignoring silence before the first
        sound (-45 dB below the peak, more than 10 ms) and dead air after the last (-60 dB, more
        than 0.5 s), with a 2 ms pre-roll so the attack is kept intact. Returns the whole range
        for silent or already-tight audio. */
    struct ContentRange { juce::int64 start = 0, end = 0; };
    ContentRange detectContent (const juce::AudioBuffer<float>&, double sampleRate);

    /** A GENERATED asset with its dead space removed (or the same pointer when there is none,
        for imported samples, for a loop that already sits on the bar grid, and for audio that
        was generated on the context). */
    AudioAsset::Ptr tighten (const AudioAsset::Ptr&, const musical::Tempo& project, double& trimmedStart, double& trimmedEnd);
}
} // namespace context
