#include "AudioAsset.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace context
{
juce::String toId (AudioAsset::Origin o)
{
    switch (o)
    {
        case AudioAsset::Origin::Generated: return "generated";
        case AudioAsset::Origin::Imported:  return "imported";
        case AudioAsset::Origin::Context:   return "context";
    }
    return "imported";
}

namespace assets
{
namespace
{
    std::unique_ptr<juce::AudioFormatManager> makeFormats()
    {
        auto m = std::make_unique<juce::AudioFormatManager>();
        m->registerBasicFormats();
        return m;
    }
}

bool isSupportedAudioFile (const juce::File& f)
{
    if (! f.existsAsFile())
        return false;
    auto formats = makeFormats();
    return formats->findFormatForFileExtension (f.getFileExtension()) != nullptr;
}

juce::String supportedWildcard()
{
    return makeFormats()->getWildcardForAllFormats();
}

LoadResult loadFile (const juce::File& file, AudioAsset::Origin origin, double maxSeconds)
{
    LoadResult r;
    if (! file.existsAsFile())
    {
        r.error = "The file couldn't be found.";
        r.detail = "missing: " + file.getFullPathName();
        return r;
    }
    auto formats = makeFormats();
    if (formats->findFormatForFileExtension (file.getFileExtension()) == nullptr)
    {
        r.error = "This file type isn't supported. Use WAV, AIFF, FLAC, OGG or MP3.";
        r.detail = "unsupported extension " + file.getFileExtension();
        return r;
    }
    std::unique_ptr<juce::AudioFormatReader> reader (formats->createReaderFor (file));
    if (reader == nullptr)
    {
        r.error = "This file couldn't be read. It may be damaged or use an unsupported encoding.";
        r.detail = "no reader for " + file.getFileName();
        return r;
    }
    if (! (reader->sampleRate >= 8000.0 && reader->sampleRate <= 384000.0))
    {
        r.error = "This file's sample rate (" + juce::String (reader->sampleRate, 0) + " Hz) isn't supported.";
        r.detail = "sample rate " + juce::String (reader->sampleRate);
        return r;
    }
    if (reader->lengthInSamples <= 0 || reader->numChannels == 0)
    {
        r.error = "This file contains no audio.";
        r.detail = "empty";
        return r;
    }
    if ((double) reader->lengthInSamples / reader->sampleRate > maxSeconds)
    {
        r.error = "This file is too long (the limit is " + juce::String ((int) (maxSeconds / 60.0)) + " minutes).";
        r.detail = "length " + juce::String ((double) reader->lengthInSamples / reader->sampleRate, 1) + " s";
        return r;
    }

    auto asset = std::make_shared<AudioAsset>();
    asset->id = juce::Uuid().toDashedString();
    asset->origin = origin;
    asset->sampleRate = reader->sampleRate;
    asset->sourceFile = file;
    asset->displayName = file.getFileName();
    // More than two channels: keep the first pair (front L/R in every common layout).
    const int channels = (int) juce::jlimit (1u, 2u, reader->numChannels);
    const int length = (int) reader->lengthInSamples;
    asset->audio.setSize (channels, length);
    if (! reader->read (&asset->audio, 0, length, 0, true, channels > 1))
    {
        r.error = "This file couldn't be decoded.";
        r.detail = "read failed";
        return r;
    }
    // Sanitise: NaN/Inf from a broken file must never reach DSP or the DAW.
    for (int ch = 0; ch < channels; ++ch)
    {
        auto* d = asset->audio.getWritePointer (ch);
        for (int i = 0; i < length; ++i)
            if (! std::isfinite (d[i]))
                d[i] = 0.0f;
    }
    r.asset = asset;
    return r;
}

bool writeWav (const juce::AudioBuffer<float>& audio, double sampleRate, const juce::File& file, int bitsPerSample, juce::String& error)
{
    if (audio.getNumSamples() <= 0 || audio.getNumChannels() <= 0 || ! (sampleRate > 0.0))
    {
        error = "Nothing to write.";
        return false;
    }
    if (! file.getParentDirectory().createDirectory())
    {
        error = "Can't create " + file.getParentDirectory().getFullPathName();
        return false;
    }
    const auto temp = file.getSiblingFile (file.getFileNameWithoutExtension() + ".writing.tmp");
    temp.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (temp);
        if (! static_cast<juce::FileOutputStream*> (stream.get())->openedOk())
        {
            error = "Can't write to " + temp.getFullPathName();
            return false;
        }
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                       .withSampleRate (sampleRate)
                                                       .withNumChannels (audio.getNumChannels())
                                                       .withBitsPerSample (bitsPerSample));
        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()))
        {
            writer.reset();
            temp.deleteFile();
            error = "Writing the WAV file failed.";
            return false;
        }
    }
    file.deleteFile();
    if (! temp.moveFileTo (file))
    {
        temp.deleteFile();
        error = "Can't move the file into place: " + file.getFullPathName();
        return false;
    }
    return true;
}

juce::AudioBuffer<float> toStereo (const juce::AudioBuffer<float>& in)
{
    juce::AudioBuffer<float> out (2, in.getNumSamples());
    if (in.getNumChannels() == 0)
    {
        out.clear();
        return out;
    }
    out.copyFrom (0, 0, in, 0, 0, in.getNumSamples());
    out.copyFrom (1, 0, in, in.getNumChannels() > 1 ? 1 : 0, 0, in.getNumSamples());
    return out;
}
} // namespace assets

// ---------------------------------------------------------------------------

WaveformPeaks::Ptr WaveformPeaks::build (const juce::AudioBuffer<float>& audio, double sampleRate, int samplesPerPeak)
{
    auto p = std::make_shared<WaveformPeaks>();
    p->spp = juce::jmax (1, samplesPerPeak);
    p->totalSamples = audio.getNumSamples();
    p->rate = sampleRate;
    const int n = (int) ((p->totalSamples + p->spp - 1) / p->spp);
    p->mins.assign ((size_t) n, 0.0f);
    p->maxs.assign ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        const int start = i * p->spp;
        const int len = juce::jmin (p->spp, audio.getNumSamples() - start);
        float lo = 0.0f, hi = 0.0f;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            const auto r = juce::FloatVectorOperations::findMinAndMax (audio.getReadPointer (ch, start), len);
            lo = juce::jmin (lo, r.getStart());
            hi = juce::jmax (hi, r.getEnd());
        }
        p->mins[(size_t) i] = lo;
        p->maxs[(size_t) i] = hi;
    }
    return p;
}

std::pair<float, float> WaveformPeaks::range (juce::int64 startSample, juce::int64 endSample) const noexcept
{
    if (mins.empty() || endSample <= startSample)
        return { 0.0f, 0.0f };
    const int a = (int) juce::jlimit<juce::int64> (0, (juce::int64) mins.size() - 1, startSample / spp);
    const int b = (int) juce::jlimit<juce::int64> (a, (juce::int64) mins.size() - 1, (endSample - 1) / spp);
    float lo = mins[(size_t) a], hi = maxs[(size_t) a];
    for (int i = a + 1; i <= b; ++i)
    {
        lo = juce::jmin (lo, mins[(size_t) i]);
        hi = juce::jmax (hi, maxs[(size_t) i]);
    }
    return { lo, hi };
}
} // namespace context
