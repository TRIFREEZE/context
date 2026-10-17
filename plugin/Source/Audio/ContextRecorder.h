#pragma once

#include "MusicalTime.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

namespace context
{
/** Records EXACTLY N musical bars of the audio arriving at the plugin input.

    How the start point is chosen (decided on the audio thread when recording begins):
      HostSync     host reports tempo + PPQ position while playing: recording starts on
                   the first bar line at or after the arm point, sample-accurately inside
                   the block, and the length comes from the host tempo.
      HostTempo    host reports tempo but no usable position: starts on the first
                   non-silent block while playing.
      Manual       no host tempo: uses the tempo given to arm() (the user's manual tempo)
                   and starts on the first non-silent block.
    In every mode the length is computed once with musical::samplesForBars() and the
    recorder stops itself after exactly that many samples.

    Realtime contract
      process() runs on the audio thread: no allocation, no locks, no I/O. The buffer is
      allocated by arm() on the message thread before the state becomes Armed, and the
      message thread only touches it again once the audio thread has published a final
      state (Complete / Failed / Idle). All hand-offs are atomic CAS transitions. */
class ContextRecorder
{
public:
    enum class State { Idle, Armed, Recording, Cancelling, Complete, Failed };
    enum class StartMode { HostSync, HostTempo, Manual };
    enum class Failure { None, TransportStopped, TempoChanged, TooLong, Cancelled };

    struct HostPosition
    {
        bool playing = false;
        bool playingKnown = false;          // host reported the play state
        std::optional<double> bpm;
        std::optional<double> ppq;
        std::optional<double> lastBarStartPpq;
        musical::TimeSignature signature;
    };

    struct Capture
    {
        juce::AudioBuffer<float> audio;     // stereo, exactly samplesForBars(bars, tempo, sampleRate) long
        double sampleRate = 0.0;
        musical::Tempo tempo;
        int bars = 0;
        StartMode mode = StartMode::Manual;
        double startPpq = -1.0;             // bar line where it started (HostSync only)
    };

    ContextRecorder() = default;

    // ---- message thread ----
    /** Allocates and arms. `fallback` is used when the host provides no tempo; it is
        also the expected tempo used to size the buffer (with headroom for slower host
        tempos). Returns false if a capture is still being cancelled. */
    bool arm (const musical::Tempo& fallback, double sampleRate, int bars = musical::contextBars);
    void cancel();
    /** Only when the audio thread is known not to run (prepareToPlay / releaseResources). */
    void resetNonRealtime();

    State getState() const noexcept           { return (State) state.load (std::memory_order_acquire); }
    Failure getFailure() const noexcept       { return (Failure) failure.load (std::memory_order_acquire); }
    StartMode getStartMode() const noexcept   { return (StartMode) startMode.load (std::memory_order_acquire); }
    double getProgress() const noexcept;      // 0..1 while recording
    int getBarsRecorded() const noexcept;     // whole bars completed so far
    int getTargetBars() const noexcept        { return targetBars.load(); }
    double getRecordingBpm() const noexcept   { return recordingBpm.load(); }

    /** When Complete: moves the capture out and returns to Idle. */
    std::optional<Capture> takeResult();
    /** When Failed: acknowledges and returns to Idle. */
    void acknowledgeFailure();

    static juce::String describe (Failure);

    // ---- audio thread ----
    void process (const float* const* input, int numChannels, int numSamples, const HostPosition&) noexcept;

    /** Peak level that counts as "music is playing" for signal-triggered starts (-60 dBFS). */
    static constexpr float signalThreshold = 0.001f;

private:
    bool tryStart (const float* const* input, int numChannels, int numSamples, const HostPosition&, int& startOffset) noexcept;
    void write (const float* const* input, int numChannels, int offset, int count) noexcept;
    void fail (Failure) noexcept;

    std::atomic<int> state { (int) State::Idle };
    std::atomic<int> failure { (int) Failure::None };
    std::atomic<int> startMode { (int) StartMode::Manual };

    // Written by arm() before Armed is published; read by the audio thread after.
    juce::AudioBuffer<float> buffer;
    double sampleRate = 44100.0;
    musical::Tempo fallbackTempo;
    std::atomic<int> targetBars { musical::contextBars };

    // Audio-thread working state, published through atomics for the UI.
    juce::int64 target = 0;
    musical::Tempo activeTempo;
    double startPpq = -1.0;
    std::atomic<juce::int64> recorded { 0 };
    std::atomic<juce::int64> targetAtomic { 0 };
    std::atomic<double> recordingBpm { 0.0 };
};
} // namespace context
