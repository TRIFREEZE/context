#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

namespace context
{
/** Rolling stereo buffer of the most recent non-silent audio routed through Context.

    Realtime contract:
      push() is called on the audio thread. It never allocates, never blocks and
      never waits: it uses SpinLock::tryEnter and simply skips the block if a
      snapshot or reallocation currently holds the lock. Capture is best-effort;
      audio passthrough is never affected.

      prepare() (allocation) runs on the message thread; snapshot() on a worker. */
class CaptureBuffer
{
public:
    CaptureBuffer() = default;

    /** Allocates capacity for `seconds` at `sampleRate`. Clears previous content. */
    void prepare (double sampleRate, int seconds);

    /** Audio thread. Blocks quieter than -100 dBFS are ignored so stopping the
        transport keeps the last N seconds of music available. */
    void push (const float* const* channels, int numChannels, int numSamples) noexcept;

    struct Snapshot
    {
        juce::AudioBuffer<float> audio;   // stereo, oldest sample first
        double sampleRate = 0.0;
        double seconds() const { return sampleRate > 0 ? audio.getNumSamples() / sampleRate : 0.0; }
    };

    /** Copies the valid part of the ring in chronological order (worker thread). */
    Snapshot snapshot() const;

    double capturedSeconds() const noexcept;
    double capacitySeconds() const noexcept;
    void clear();

    static constexpr float silenceThreshold = 1.0e-5f;   // -100 dBFS

private:
    mutable juce::SpinLock lock;
    juce::AudioBuffer<float> ring;          // guarded by lock
    int writePos = 0;                        // guarded by lock
    int validSamples = 0;                    // guarded by lock
    double sampleRate = 0.0;                 // guarded by lock
    std::atomic<int> validAtomic { 0 };
    std::atomic<double> rateAtomic { 0.0 };
    std::atomic<int> capacityAtomic { 0 };
};
} // namespace context
