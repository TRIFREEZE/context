#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace context::dsp
{
/** Offline, high-quality band-limited resampling (Kaiser-windowed sinc, 32 zero
    crossings, ~-90 dB stop band). When the output rate is lower than the input rate
    the kernel is widened so content above the new Nyquist is filtered out instead of
    aliasing. Not realtime: runs on worker threads. */
namespace resampler
{
    /** Sample-rate conversion: output length = llround (inLength * outRate / inRate). */
    juce::AudioBuffer<float> convertRate (const juce::AudioBuffer<float>& in, double inRate, double outRate);

    /** Stretches/squeezes the whole buffer to exactly `outLength` samples (tape-style:
        pitch moves with speed). Used for REPITCH. */
    juce::AudioBuffer<float> toLength (const juce::AudioBuffer<float>& in, juce::int64 outLength);

    /** Resamples one channel reading input positions `start + n * step`. */
    void process (const float* in, int inLength, float* out, int outLength, double start, double step);
}
} // namespace context::dsp
