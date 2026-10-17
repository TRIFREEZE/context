#include "CaptureBuffer.h"

namespace context
{
void CaptureBuffer::prepare (double newRate, int seconds)
{
    if (newRate <= 0.0 || ! std::isfinite (newRate))
        newRate = 44100.0;
    const int capacity = juce::jlimit (1024, 30 * 384000, (int) (newRate * juce::jlimit (1, 30, seconds)));

    juce::AudioBuffer<float> fresh (2, capacity);   // allocate OUTSIDE the lock
    fresh.clear();

    const juce::SpinLock::ScopedLockType sl (lock);
    std::swap (ring, fresh);
    writePos = 0;
    validSamples = 0;
    sampleRate = newRate;
    validAtomic.store (0);
    rateAtomic.store (newRate);
    capacityAtomic.store (capacity);
    // `fresh` (old buffer) is freed when this function returns, outside realtime code.
}

void CaptureBuffer::push (const float* const* channels, int numChannels, int numSamples) noexcept
{
    if (numChannels <= 0 || numSamples <= 0 || channels == nullptr)
        return;

    // Cheap silence test (no locks, no allocation).
    float peak = 0.0f;
    for (int ch = 0; ch < juce::jmin (numChannels, 2); ++ch)
        if (channels[ch] != nullptr)
            peak = juce::jmax (peak, juce::FloatVectorOperations::findMaximum (channels[ch], numSamples),
                                    -juce::FloatVectorOperations::findMinimum (channels[ch], numSamples));
    if (! (peak > silenceThreshold))     // also rejects NaN
        return;

    const juce::SpinLock::ScopedTryLockType tl (lock);
    if (! tl.isLocked())
        return;                          // snapshot/realloc in progress: skip this block

    const int capacity = ring.getNumSamples();
    if (capacity <= 0)
        return;

    const float* left  = channels[0];
    const float* right = (numChannels > 1 && channels[1] != nullptr) ? channels[1] : channels[0];
    if (left == nullptr)
        return;

    int remaining = juce::jmin (numSamples, capacity);
    int srcOffset = numSamples - remaining;   // if a block is longer than the ring keep its tail
    while (remaining > 0)
    {
        const int chunk = juce::jmin (remaining, capacity - writePos);
        ring.copyFrom (0, writePos, left + srcOffset, chunk);
        ring.copyFrom (1, writePos, right + srcOffset, chunk);
        writePos = (writePos + chunk) % capacity;
        srcOffset += chunk;
        remaining -= chunk;
    }
    validSamples = juce::jmin (capacity, validSamples + numSamples);
    validAtomic.store (validSamples, std::memory_order_relaxed);
}

CaptureBuffer::Snapshot CaptureBuffer::snapshot() const
{
    Snapshot s;
    // Allocate the destination at the current size before taking the lock.
    const int expected = validAtomic.load();
    s.audio.setSize (2, juce::jmax (1, expected), false, false, false);

    const juce::SpinLock::ScopedLockType sl (lock);
    const int capacity = ring.getNumSamples();
    const int n = juce::jmin (validSamples, s.audio.getNumSamples());
    s.sampleRate = sampleRate;
    if (n <= 0 || capacity <= 0)
    {
        s.audio.setSize (2, 0);
        return s;
    }

    const int start = (writePos - n + capacity) % capacity;
    const int first = juce::jmin (n, capacity - start);
    for (int ch = 0; ch < 2; ++ch)
    {
        s.audio.copyFrom (ch, 0, ring, ch, start, first);
        if (n > first)
            s.audio.copyFrom (ch, first, ring, ch, 0, n - first);
    }
    if (n < s.audio.getNumSamples())
        s.audio.setSize (2, n, true, false, true);
    return s;
}

double CaptureBuffer::capturedSeconds() const noexcept
{
    const double r = rateAtomic.load();
    return r > 0.0 ? validAtomic.load() / r : 0.0;
}

double CaptureBuffer::capacitySeconds() const noexcept
{
    const double r = rateAtomic.load();
    return r > 0.0 ? capacityAtomic.load() / r : 0.0;
}

void CaptureBuffer::clear()
{
    const juce::SpinLock::ScopedLockType sl (lock);
    ring.clear();
    writePos = 0;
    validSamples = 0;
    validAtomic.store (0);
}
} // namespace context
