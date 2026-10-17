#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <atomic>

namespace context
{
/** Plays a generated file through the plugin's output for auditioning.

    Realtime design:
      - decoding happens off the audio thread (load())
      - the decoded clip is handed to the audio thread through an atomic pointer
      - the audio thread never allocates or frees: replaced clips are pushed to a
        lock-free FIFO and deleted later on the message thread (collectGarbage())
      - mixing is a simple add with gain; resampling is linear interpolation, so
        host sample-rate changes need no reload
      - never mixed during offline rendering (bounces stay clean) */
class PreviewPlayer
{
public:
    PreviewPlayer();
    ~PreviewPlayer();   // call only when the audio thread is not running (processor destruction)

    // ---- worker thread (decoding) ----
    bool load (const juce::File& file, juce::String& error);   // decodes (max 10 minutes)

    // ---- message thread ----
    void play();
    void stop();
    void setGainDb (float db);
    void collectGarbage();                                       // the ONLY consumer of the retired FIFO

    bool isPlaying() const noexcept       { return playing.load(); }
    double positionSeconds() const noexcept;
    double lengthSeconds() const noexcept { return loadedLength.load(); }
    juce::File loadedFile() const;

    // ---- audio thread ----
    void setHostSampleRate (double sr) noexcept { hostRate.store (sr > 0 ? sr : 44100.0); }
    void renderAdd (juce::AudioBuffer<float>& out, int numSamples, bool isOfflineRender) noexcept;

private:
    struct Clip
    {
        juce::AudioBuffer<float> audio;
        double sampleRate = 44100.0;
    };

    std::atomic<Clip*> pending { nullptr };
    Clip* current = nullptr;                      // audio thread only
    double readPos = 0.0;                         // audio thread only

    static constexpr int retiredCapacity = 16;
    juce::AbstractFifo retiredFifo { retiredCapacity };
    std::array<Clip*, retiredCapacity> retired {};

    std::atomic<bool> playRequested { false }, stopRequested { false }, playing { false };
    std::atomic<float> gain { 0.5f };
    std::atomic<double> hostRate { 44100.0 };
    std::atomic<double> positionSec { 0.0 };
    std::atomic<double> loadedLength { 0.0 };

    mutable juce::CriticalSection fileLock;
    juce::File file;
};
} // namespace context
