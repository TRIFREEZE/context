#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace context
{
/** Proves a downloaded file is real, decodable audio using an actual decoder
    (the extension and server headers are not trusted). WAV and AIFF accepted. */
struct AudioFileInfo
{
    bool ok = false;
    juce::String error;
    juce::String formatName;         // "WAV" / "AIFF"
    juce::String extension;          // ".wav" / ".aiff"
    double sampleRate = 0.0;
    int channels = 0;
    juce::int64 lengthInSamples = 0;
    double durationSeconds() const { return sampleRate > 0 ? (double) lengthInSamples / sampleRate : 0.0; }
};

class AudioFileValidator
{
public:
    static constexpr juce::int64 maxFileBytes = 100 * 1024 * 1024;   // 100 MB
    static constexpr double maxDurationSeconds = 20.0 * 60.0;

    /** Opens with WAV then AIFF decoders and reads every sample block to make sure
        the whole file decodes. Never throws. */
    static AudioFileInfo validate (const juce::File& file);
};
} // namespace context
