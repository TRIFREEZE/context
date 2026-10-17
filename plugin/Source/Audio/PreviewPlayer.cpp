#include "PreviewPlayer.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace context
{
PreviewPlayer::PreviewPlayer() = default;

PreviewPlayer::~PreviewPlayer()
{
    delete pending.exchange (nullptr);
    delete current;
    current = nullptr;
    collectGarbage();
}

bool PreviewPlayer::load (const juce::File& f, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerFormat (new juce::WavAudioFormat(), true);
    formats.registerFormat (new juce::AiffAudioFormat(), false);

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));
    if (reader == nullptr)
    {
        error = "Generated audio could not be decoded.";
        return false;
    }
    if (reader->lengthInSamples <= 0 || reader->sampleRate <= 0 || reader->lengthInSamples > (juce::int64) (reader->sampleRate * 600.0))
    {
        error = "This file is empty or too long to preview.";
        return false;
    }

    auto clip = std::make_unique<Clip>();
    const int channels = (int) juce::jlimit (1u, 2u, reader->numChannels);
    clip->audio.setSize (channels, (int) reader->lengthInSamples);
    clip->sampleRate = reader->sampleRate;
    if (! reader->read (&clip->audio, 0, (int) reader->lengthInSamples, 0, true, channels > 1))
    {
        error = "Generated audio could not be decoded.";
        return false;
    }

    stopRequested = true;
    loadedLength = clip->audio.getNumSamples() / clip->sampleRate;
    {
        const juce::ScopedLock sl (fileLock);
        file = f;
    }
    delete pending.exchange (clip.release());   // an unconsumed older clip was never seen by the audio thread
    return true;
}

void PreviewPlayer::play()
{
    stopRequested = false;
    playRequested = true;
    playing = true;   // optimistic, so the UI updates immediately
}

void PreviewPlayer::stop()
{
    playRequested = false;
    stopRequested = true;
    playing = false;
}

void PreviewPlayer::setGainDb (float db)
{
    gain.store (juce::Decibels::decibelsToGain (juce::jlimit (-60.0f, 0.0f, db)));
}

double PreviewPlayer::positionSeconds() const noexcept { return positionSec.load(); }

juce::File PreviewPlayer::loadedFile() const
{
    const juce::ScopedLock sl (fileLock);
    return file;
}

void PreviewPlayer::collectGarbage()
{
    const auto scope = retiredFifo.read (retiredFifo.getNumReady());
    for (int i = 0; i < scope.blockSize1; ++i) { delete retired[(size_t) (scope.startIndex1 + i)]; retired[(size_t) (scope.startIndex1 + i)] = nullptr; }
    for (int i = 0; i < scope.blockSize2; ++i) { delete retired[(size_t) (scope.startIndex2 + i)]; retired[(size_t) (scope.startIndex2 + i)] = nullptr; }
}

void PreviewPlayer::renderAdd (juce::AudioBuffer<float>& out, int numSamples, bool isOfflineRender) noexcept
{
    // Swap in a newly loaded clip if there is room to retire the old one.
    if (pending.load (std::memory_order_acquire) != nullptr && retiredFifo.getFreeSpace() > 0)
    {
        if (Clip* incoming = pending.exchange (nullptr))
        {
            if (current != nullptr)
            {
                const auto scope = retiredFifo.write (1);
                if (scope.blockSize1 > 0)      retired[(size_t) scope.startIndex1] = current;
                else if (scope.blockSize2 > 0) retired[(size_t) scope.startIndex2] = current;
            }
            current = incoming;
            readPos = 0.0;
            positionSec.store (0.0);
        }
    }

    if (stopRequested.exchange (false))
    {
        playing.store (false);
        readPos = 0.0;
        positionSec.store (0.0);
    }
    if (playRequested.exchange (false))
    {
        readPos = 0.0;
        playing.store (current != nullptr);
    }

    if (! playing.load() || current == nullptr || numSamples <= 0)
        return;
    if (isOfflineRender)
        return;   // keep bounces/exports free of preview audio

    const auto& clip = current->audio;
    const int clipLen = clip.getNumSamples();
    const int clipChans = clip.getNumChannels();
    const int outChans = out.getNumChannels();
    const double step = current->sampleRate / hostRate.load();
    const float g = gain.load();

    int i = 0;
    for (; i < numSamples; ++i)
    {
        const int idx = (int) readPos;
        if (idx >= clipLen - 1)
        {
            playing.store (false);
            break;
        }
        const float frac = (float) (readPos - idx);
        const float l = clip.getSample (0, idx) + frac * (clip.getSample (0, idx + 1) - clip.getSample (0, idx));
        const float r = clipChans > 1 ? clip.getSample (1, idx) + frac * (clip.getSample (1, idx + 1) - clip.getSample (1, idx)) : l;

        if (outChans == 1)
            out.addSample (0, i, 0.5f * (l + r) * g);
        else
        {
            out.addSample (0, i, l * g);
            out.addSample (1, i, r * g);
        }
        readPos += step;
    }
    positionSec.store (readPos / current->sampleRate);
}
} // namespace context
