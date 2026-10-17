#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <optional>

namespace context
{
/** A piece of audio Context works with: the captured track context, a generated
    result or a sample the user dropped in.

    Assets are IMMUTABLE once created and shared through AudioAsset::Ptr
    (shared_ptr<const AudioAsset>). Any thread may read one at any time; "changing"
    an asset means building a new one and swapping the pointer. This is what makes
    it safe for the UI, workers and renderer to use the same audio concurrently.

    Generated and imported audio are the same type with a different `origin`, so
    both go through exactly the same fit -> effects -> preview -> export pipeline. */
struct AudioAsset
{
    using Ptr = std::shared_ptr<const AudioAsset>;
    enum class Origin { Generated, Imported, Context };

    juce::String id;                     // unique per asset instance
    Origin origin = Origin::Imported;
    juce::AudioBuffer<float> audio;      // 1 or 2 channels, never more
    double sampleRate = 0.0;

    juce::String displayName;            // file name / prompt
    juce::File sourceFile;               // where it came from (may be empty for captures)
    juce::String generationId;           // history id for generated assets

    // Musical facts that are KNOWN (requested from the generator, read from the
    // file name, or measured by the host). Estimates live in the fit analysis.
    std::optional<double> bpm;
    juce::String key, mode;

    int numChannels() const noexcept         { return audio.getNumChannels(); }
    juce::int64 numSamples() const noexcept  { return audio.getNumSamples(); }
    double seconds() const noexcept          { return sampleRate > 0.0 ? audio.getNumSamples() / sampleRate : 0.0; }
    bool isValid() const noexcept            { return sampleRate > 0.0 && audio.getNumSamples() > 0 && audio.getNumChannels() > 0; }
};

juce::String toId (AudioAsset::Origin);

namespace assets
{
    /** Longest audio Context will load into a lane. */
    inline constexpr double maxLoadSeconds = 10.0 * 60.0;

    struct LoadResult
    {
        AudioAsset::Ptr asset;
        juce::String error;          // short, human readable ("This file type isn't supported.")
        juce::String detail;         // for the log
        bool ok() const { return asset != nullptr; }
    };

    /** Decodes any format JUCE supports on this platform (WAV, AIFF, FLAC, Ogg; MP3/AAC
        through the OS decoders on Windows and macOS). Worker thread only. */
    LoadResult loadFile (const juce::File&, AudioAsset::Origin, double maxSeconds = maxLoadSeconds);

    /** True if the file has an extension one of the platform's audio formats claims. */
    bool isSupportedAudioFile (const juce::File&);
    juce::String supportedWildcard();

    /** Writes a WAV (24-bit by default) via a temp file + move, so a crash never leaves
        a half-written file under the final name. */
    bool writeWav (const juce::AudioBuffer<float>&, double sampleRate, const juce::File&, int bitsPerSample, juce::String& error);

    /** Copies into a new 2-channel buffer (mono is duplicated at equal level, not +3 dB). */
    juce::AudioBuffer<float> toStereo (const juce::AudioBuffer<float>&);
}

/** Min/max peak pyramid for fast waveform drawing (built on a worker, drawn on the
    message thread without touching sample data). */
class WaveformPeaks
{
public:
    using Ptr = std::shared_ptr<const WaveformPeaks>;
    static Ptr build (const juce::AudioBuffer<float>&, double sampleRate, int samplesPerPeak = 64);

    int samplesPerPeak() const noexcept  { return spp; }
    juce::int64 numSamples() const noexcept { return totalSamples; }
    double sampleRate() const noexcept   { return rate; }
    int numPeaks() const noexcept        { return (int) mins.size(); }

    /** Min/max over the sample range [start, end) (clamped). */
    std::pair<float, float> range (juce::int64 startSample, juce::int64 endSample) const noexcept;

private:
    std::vector<float> mins, maxs;
    int spp = 64;
    juce::int64 totalSamples = 0;
    double rate = 0.0;
};
} // namespace context
