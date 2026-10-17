#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "Utils/CancellationToken.h"
#include <functional>
#include <memory>
#include <optional>

namespace context::stretch
{
/** Stretch algorithms offered to the user. Each one is a genuinely different
    implementation or parameterisation (see createEngine):
      Percussive   transient slicing: attacks copied verbatim, only the decays between
                   hits are time-scaled (WSOLA) -> drums stay punchy.
      Monophonic   WSOLA with long frames and a wide similarity search -> keeps the pitch
                   periods of voices, bass and leads intact.
      Polyphonic   phase vocoder (4096-point, 4x overlap) with identity phase locking and
                   transient phase reset -> chords, guitars, full loops.
      Texture      phase vocoder (8192-point, 8x overlap), no transient handling -> very
                   smooth for pads and ambience.
      Repitch      band-limited resampling: tempo and pitch move together (tape/sampler).
      Auto         analyses the material and picks one of the above. */
enum class Algorithm { Auto, Percussive, Monophonic, Polyphonic, Texture, Repitch };

juce::String toId (Algorithm);
juce::String toLabel (Algorithm);
juce::String tooltipFor (Algorithm);
std::optional<Algorithm> algorithmFromId (const juce::String&);
inline constexpr Algorithm allAlgorithms[] = { Algorithm::Auto, Algorithm::Percussive, Algorithm::Monophonic,
                                              Algorithm::Polyphonic, Algorithm::Texture, Algorithm::Repitch };

/** Cancellation + progress reporting for offline renders. */
struct StretchProgress
{
    const CancellationToken* cancel = nullptr;
    std::function<void (float)> report;     // 0..1, called from the rendering thread
    bool cancelled() const noexcept { return cancel != nullptr && cancel->isCancelled(); }
};

/** Offline time-stretch / pitch-shift engine. Implementations are swappable: the rest
    of Context only ever talks to this interface (a licensed third-party engine could be
    dropped in behind it without touching the fit renderer or UI). */
class TimeStretchEngine
{
public:
    virtual ~TimeStretchEngine() = default;

    virtual juce::String name() const = 0;
    virtual void prepare (double sampleRate, int numChannels) = 0;
    /** Output length / input length (2.0 = twice as long, i.e. half speed). */
    virtual void setRatio (double timeRatio) = 0;
    /** Pitch shift in semitones, independent of the time ratio (ignored by Repitch). */
    virtual void setPitchSemitones (double semitones) = 0;

    using Progress = StretchProgress;

    /** Processes a whole buffer offline. The result has exactly
        llround (input length * ratio) samples and the same channel count. */
    virtual juce::AudioBuffer<float> process (const juce::AudioBuffer<float>& input, const Progress& = {}) = 0;
};

std::unique_ptr<TimeStretchEngine> createEngine (Algorithm resolved);

/** Material analysis used by Auto (and shown to the user as "Auto: Percussive"). */
struct MaterialAnalysis
{
    double onsetsPerSecond = 0.0;
    double periodicity = 0.0;        // 0..1 normalised autocorrelation of pitch periods
    double flatness = 0.0;           // 0..1 spectral flatness (noisiness)
    Algorithm suggested = Algorithm::Polyphonic;
};
MaterialAnalysis analyseMaterial (const juce::AudioBuffer<float>&, double sampleRate);

/** Sample positions of note/drum onsets (energy-flux detector, ~3 ms resolution). */
std::vector<int> detectOnsets (const juce::AudioBuffer<float>&, double sampleRate, double minGapSeconds = 0.05);
} // namespace context::stretch
