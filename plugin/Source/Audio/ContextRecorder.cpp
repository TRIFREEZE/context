#include "ContextRecorder.h"

namespace context
{
namespace
{
    constexpr double minimumSizingBpm = 40.0;
}

bool ContextRecorder::arm (const musical::Tempo& fallback, double rate, int bars)
{
    auto s = getState();
    if (s == State::Armed || s == State::Recording || s == State::Cancelling)
        return false;

    const double sr = rate > 0.0 && std::isfinite (rate) ? rate : 44100.0;
    auto tempo = fallback.isValid() ? fallback : musical::Tempo{};
    // Size for the slowest plausible host tempo: half the expected tempo, never below 40 BPM.
    const musical::Tempo sizing { juce::jmax (minimumSizingBpm, tempo.bpm * 0.5), tempo.signature };
    const auto capacity = musical::samplesForBars (bars, sizing, sr) + 16384;
    const auto maxCapacity = (juce::int64) (sr * 120.0);   // 2 minutes of audio is the hard ceiling
    juce::AudioBuffer<float> fresh (2, (int) juce::jmin (capacity, maxCapacity));
    fresh.clear();

    buffer = std::move (fresh);
    sampleRate = sr;
    fallbackTempo = tempo;
    targetBars.store (juce::jlimit (1, 64, bars));
    target = 0;
    startPpq = -1.0;
    recorded.store (0);
    targetAtomic.store (0);
    recordingBpm.store (0.0);
    failure.store ((int) Failure::None);
    state.store ((int) State::Armed, std::memory_order_release);
    return true;
}

void ContextRecorder::cancel()
{
    int expected = (int) State::Armed;
    if (state.compare_exchange_strong (expected, (int) State::Idle, std::memory_order_acq_rel))
        return;
    expected = (int) State::Recording;
    if (state.compare_exchange_strong (expected, (int) State::Cancelling, std::memory_order_acq_rel))
        return;   // the audio thread acknowledges by returning to Idle
    expected = (int) State::Complete;
    state.compare_exchange_strong (expected, (int) State::Idle, std::memory_order_acq_rel);
    expected = (int) State::Failed;
    state.compare_exchange_strong (expected, (int) State::Idle, std::memory_order_acq_rel);
}

void ContextRecorder::resetNonRealtime()
{
    state.store ((int) State::Idle, std::memory_order_release);
    recorded.store (0);
}

double ContextRecorder::getProgress() const noexcept
{
    const auto t = targetAtomic.load();
    return t > 0 ? juce::jlimit (0.0, 1.0, (double) recorded.load() / (double) t) : 0.0;
}

int ContextRecorder::getBarsRecorded() const noexcept
{
    return (int) std::floor (getProgress() * targetBars.load() + 1.0e-9);
}

std::optional<ContextRecorder::Capture> ContextRecorder::takeResult()
{
    if (getState() != State::Complete)
        return std::nullopt;
    Capture c;
    c.sampleRate = sampleRate;
    c.tempo = activeTempo;
    c.bars = targetBars.load();
    c.mode = getStartMode();
    c.startPpq = startPpq;
    c.audio = std::move (buffer);
    c.audio.setSize (2, (int) target, true, false, false);
    buffer = juce::AudioBuffer<float>();
    state.store ((int) State::Idle, std::memory_order_release);
    return c;
}

void ContextRecorder::acknowledgeFailure()
{
    int expected = (int) State::Failed;
    state.compare_exchange_strong (expected, (int) State::Idle, std::memory_order_acq_rel);
}

juce::String ContextRecorder::describe (Failure f)
{
    switch (f)
    {
        case Failure::None:             return {};
        case Failure::TransportStopped: return "Playback stopped before all bars were captured. Press Capture and play again.";
        case Failure::TempoChanged:     return "The tempo changed during the capture. Capture again at a steady tempo.";
        case Failure::TooLong:          return "At this tempo the capture would be too long. Check the project tempo.";
        case Failure::Cancelled:        return "Capture cancelled.";
    }
    return {};
}

void ContextRecorder::fail (Failure f) noexcept
{
    failure.store ((int) f, std::memory_order_release);
    state.store ((int) State::Failed, std::memory_order_release);
}

bool ContextRecorder::tryStart (const float* const* input, int numChannels, int numSamples, const HostPosition& host, int& startOffset) noexcept
{
    const bool hostTempo = host.bpm.has_value() && musical::isValidBpm (*host.bpm);
    const bool hostPlaying = host.playingKnown ? host.playing : true;

    if (hostTempo && host.ppq.has_value() && host.playingKnown)
    {
        if (! host.playing)
            return false;
        // HostSync: first bar line at or after this block's start.
        const auto ts = host.signature.isValid() ? host.signature : musical::TimeSignature{};
        const double ppq = *host.ppq;
        const double bar = musical::nextBarStartPpq (ppq, ts, host.lastBarStartPpq);
        const double samplesPerQuarter = sampleRate * 60.0 / *host.bpm;
        const double offset = (bar - ppq) * samplesPerQuarter;
        if (offset < 0.0 || offset >= numSamples)
            return false;
        const auto rounded = std::llround (offset);
        if (rounded >= numSamples)
            return false;   // the bar line falls on the next block's first sample
        startOffset = (int) rounded;
        activeTempo = { *host.bpm, ts };
        startPpq = bar;
        startMode.store ((int) StartMode::HostSync);
        return true;
    }

    if (! hostPlaying)
        return false;

    // Signal-triggered start (no usable host position).
    float peak = 0.0f;
    int firstLoud = -1;
    for (int ch = 0; ch < juce::jmin (2, numChannels) && firstLoud < 0; ++ch)
        if (input[ch] != nullptr)
            for (int i = 0; i < numSamples; ++i)
            {
                peak = juce::jmax (peak, std::abs (input[ch][i]));
                if (peak > signalThreshold) { firstLoud = i; break; }
            }
    if (firstLoud < 0)
        return false;

    startOffset = firstLoud;
    if (hostTempo)
    {
        activeTempo = { *host.bpm, host.signature.isValid() ? host.signature : fallbackTempo.signature };
        startMode.store ((int) StartMode::HostTempo);
    }
    else
    {
        activeTempo = fallbackTempo;
        startMode.store ((int) StartMode::Manual);
    }
    startPpq = -1.0;
    return true;
}

void ContextRecorder::write (const float* const* input, int numChannels, int offset, int count) noexcept
{
    const auto pos = recorded.load (std::memory_order_relaxed);
    const int n = (int) juce::jmin<juce::int64> (count, target - pos);
    if (n <= 0)
        return;
    const float* left = numChannels > 0 ? input[0] : nullptr;
    const float* right = numChannels > 1 && input[1] != nullptr ? input[1] : left;
    if (left == nullptr)
    {
        buffer.clear (0, (int) pos, n);
        buffer.clear (1, (int) pos, n);
    }
    else
    {
        buffer.copyFrom (0, (int) pos, left + offset, n);
        buffer.copyFrom (1, (int) pos, right + offset, n);
    }
    recorded.store (pos + n, std::memory_order_relaxed);
}

void ContextRecorder::process (const float* const* input, int numChannels, int numSamples, const HostPosition& host) noexcept
{
    auto s = (State) state.load (std::memory_order_acquire);
    if (s == State::Cancelling)
    {
        failure.store ((int) Failure::Cancelled);
        state.store ((int) State::Idle, std::memory_order_release);
        return;
    }
    if ((s != State::Armed && s != State::Recording) || numSamples <= 0 || input == nullptr)
        return;

    int offset = 0;
    if (s == State::Armed)
    {
        if (! tryStart (input, numChannels, numSamples, host, offset))
            return;
        target = musical::samplesForBars (targetBars.load(), activeTempo, sampleRate);
        if (target <= 0 || target > buffer.getNumSamples())
        {
            fail (Failure::TooLong);
            return;
        }
        recorded.store (0);
        targetAtomic.store (target);
        recordingBpm.store (activeTempo.bpm);
        int expected = (int) State::Armed;
        if (! state.compare_exchange_strong (expected, (int) State::Recording, std::memory_order_acq_rel))
            return;   // cancelled concurrently
    }
    else
    {
        // While recording, the musical frame must stay what we started with.
        if (getStartMode() != StartMode::Manual)
        {
            if (host.playingKnown && ! host.playing)
            {
                fail (Failure::TransportStopped);
                return;
            }
            if (host.bpm.has_value() && std::abs (*host.bpm - activeTempo.bpm) > 0.01)
            {
                fail (Failure::TempoChanged);
                return;
            }
        }
    }

    write (input, numChannels, offset, numSamples - offset);
    if (recorded.load (std::memory_order_relaxed) >= target)
    {
        int expected = (int) State::Recording;
        state.compare_exchange_strong (expected, (int) State::Complete, std::memory_order_acq_rel);
    }
}
} // namespace context
