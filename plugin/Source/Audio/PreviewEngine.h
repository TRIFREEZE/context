#pragma once

#include "DSP/EffectChain.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <array>

namespace context
{
/** Plays the TRACK CONTEXT lane and the AI / SAMPLE lane together on one shared,
    sample-counted timeline, inside the plugin (independent of the DAW transport).

    Timeline
      Both lanes are pre-rendered at the host sample rate. The timeline is as long as
      the longer lane; the shorter lane repeats (a 4-bar sample under an 8-bar context
      plays twice). Positions are integer sample counts, so lanes can never drift.

    Output lane signal flow
      fitted source -> EffectChain (realtime) -> lane gain -> out
      The chain's latency is compensated by reading the source `latency` samples ahead,
      so the processed output lines up exactly with the context lane and the plugin's
      own (passthrough) latency stays zero. ORIGINAL/PROCESSED compare crossfades to the
      unprocessed source read at the aligned position.

    Realtime contract
      Programs (the two buffers) arrive through an atomic pointer; replaced programs go
      to a retire FIFO and are freed by collectGarbage() on the message thread. Gains
      are smoothed; start/stop and program swaps are crossfaded. Nothing is mixed during
      offline renders (bounces stay clean). */
class PreviewEngine
{
public:
    struct Program
    {
        std::shared_ptr<const juce::AudioBuffer<float>> context;   // stereo or null
        std::shared_ptr<const juce::AudioBuffer<float>> output;    // stereo or null
        double sampleRate = 0.0;
        juce::uint64 serial = 0;

        juce::int64 contextLength() const noexcept { return context != nullptr ? context->getNumSamples() : 0; }
        juce::int64 outputLength() const noexcept  { return output != nullptr ? output->getNumSamples() : 0; }
        juce::int64 timelineLength() const noexcept { return juce::jmax (contextLength(), outputLength()); }
    };

    enum class Mode { Both, Context, Output };
    enum Lane { contextLane = 0, outputLane = 1 };

    PreviewEngine();
    ~PreviewEngine();   // only when the audio thread no longer runs

    // ---- message thread ----
    void setProgram (std::unique_ptr<Program>);
    void play();
    void stop();
    void rewind();
    void setLooping (bool b)                { looping.store (b); }
    void setMode (Mode m)                   { mode.store ((int) m); }
    void setLaneGainDb (int lane, float db);
    void setLaneMute (int lane, bool m)     { mute[(size_t) lane].store (m); }
    void setLaneSolo (int lane, bool s)     { solo[(size_t) lane].store (s); }
    void setCompareOriginal (bool b)        { compareOriginal.store (b); }
    void setMasterGainDb (float db);
    void setTempo (double bpm)              { tempoBpm.store (bpm); }
    void collectGarbage();

    bool isPlaying() const noexcept         { return playing.load(); }
    bool isLooping() const noexcept         { return looping.load(); }
    Mode getMode() const noexcept           { return (Mode) mode.load(); }
    juce::int64 position() const noexcept   { return positionAtomic.load(); }
    juce::int64 timelineLength() const noexcept { return timelineAtomic.load(); }
    double programRate() const noexcept     { return rateAtomic.load(); }
    bool laneAudible (int lane) const noexcept;

    // ---- non-realtime ----
    void prepare (double sampleRate, int maxBlockSize);

    // ---- audio thread ----
    void renderAdd (juce::AudioBuffer<float>& out, int numSamples, bool offline, fx::EffectChain& chain) noexcept;

private:
    void swapProgramIfPending() noexcept;
    void readLane (const juce::AudioBuffer<float>& src, juce::int64 pos, juce::AudioBuffer<float>& dest, int destStart, int n) const noexcept;
    void readWithSwap (int lane, juce::int64 at, juce::AudioBuffer<float>& dest, int n) noexcept;
    void retire (Program*) noexcept;
    float laneTarget (int lane) const noexcept;

    std::atomic<Program*> pending { nullptr };
    Program* current = nullptr;          // audio thread
    Program* fadingOut = nullptr;        // audio thread: previous program during a swap crossfade
    int swapFadePos = 0;
    static constexpr int retireCapacity = 32;
    juce::AbstractFifo retireFifo { retireCapacity };
    std::array<Program*, retireCapacity> retired {};

    std::atomic<bool> playRequested { false }, stopRequested { false }, rewindRequested { false };
    std::atomic<bool> playing { false }, looping { true }, compareOriginal { false };
    std::atomic<int> mode { (int) Mode::Both };
    std::array<std::atomic<float>, 2> laneGain { 1.0f, 1.0f };
    std::array<std::atomic<bool>, 2> mute { false, false }, solo { false, false };
    std::atomic<float> masterGain { 1.0f };
    std::atomic<double> tempoBpm { 120.0 };

    std::atomic<juce::int64> positionAtomic { 0 }, timelineAtomic { 0 };
    std::atomic<double> rateAtomic { 0.0 };

    // audio-thread state
    juce::int64 pos = 0;
    bool active = false;                 // producing sound (including the stop fade)
    bool stopping = false;
    bool outputWasAudible = true;
    int stopFadeRemaining = 0;
    float startRamp = 1.0f;
    std::array<juce::SmoothedValue<float>, 2> laneSmooth;
    juce::SmoothedValue<float> compareSmooth, masterSmooth;
    juce::AudioBuffer<float> ctxBuf, outBuf, dryBuf, oldBuf;
    int maxBlock = 0;
    double hostRate = 44100.0;
    int fadeSamples = 256;
};
} // namespace context
