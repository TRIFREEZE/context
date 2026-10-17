#include "AudioFileValidator.h"

namespace context
{
AudioFileInfo AudioFileValidator::validate (const juce::File& file)
{
    AudioFileInfo info;
    try
    {
        if (! file.existsAsFile())     { info.error = "File does not exist"; return info; }
        const auto size = file.getSize();
        if (size <= 44)                { info.error = "File is empty or truncated (" + juce::String (size) + " bytes)"; return info; }
        if (size > maxFileBytes)       { info.error = "File exceeds 100 MB"; return info; }

        std::unique_ptr<juce::AudioFormatReader> reader;
        juce::WavAudioFormat wav;
        juce::AiffAudioFormat aiff;

        for (juce::AudioFormat* fmt : { static_cast<juce::AudioFormat*> (&wav), static_cast<juce::AudioFormat*> (&aiff) })
        {
            auto stream = file.createInputStream();
            if (stream == nullptr) { info.error = "Cannot open file"; return info; }
            reader.reset (fmt->createReaderFor (stream.release(), true));
            if (reader != nullptr)
            {
                info.formatName = fmt == &wav ? "WAV" : "AIFF";
                info.extension  = fmt == &wav ? ".wav" : ".aiff";
                break;
            }
        }

        if (reader == nullptr)                     { info.error = "Not a WAV or AIFF file"; return info; }
        info.sampleRate = reader->sampleRate;
        info.channels = (int) reader->numChannels;
        info.lengthInSamples = reader->lengthInSamples;

        if (info.sampleRate < 8000.0 || info.sampleRate > 384000.0) { info.error = "Unsupported sample rate"; return info; }
        if (info.channels < 1 || info.channels > 8)                  { info.error = "Unsupported channel count"; return info; }
        if (info.lengthInSamples <= 0)                               { info.error = "Audio contains no samples"; return info; }
        if (info.durationSeconds() > maxDurationSeconds)             { info.error = "Audio is longer than 20 minutes"; return info; }

        // Decode everything in blocks to prove the data is readable.
        juce::AudioBuffer<float> block (info.channels, 32768);
        for (juce::int64 pos = 0; pos < info.lengthInSamples; pos += block.getNumSamples())
        {
            const int n = (int) juce::jmin ((juce::int64) block.getNumSamples(), info.lengthInSamples - pos);
            if (! reader->read (&block, 0, n, pos, true, true))
            {
                info.error = "Decoder failed at sample " + juce::String (pos);
                return info;
            }
        }

        info.ok = true;
    }
    catch (...)
    {
        info.ok = false;
        info.error = "Decoder threw an exception";
    }
    return info;
}
} // namespace context
