#include "PreviewEngine.h"

namespace context
{
PreviewEngine::PreviewEngine()
{
    for (auto& s : laneSmooth)
        s.setCurrentAndTargetValue (1.0f);
    compareSmooth.setCurrentAndTargetValue (0.0f);
    masterSmooth.setCurrentAndTargetValue (1.0f);
}

PreviewEngine::~PreviewEngine()
{
    delete pending.exchange (nullptr);
    delete current;
    current = nullptr;
    delete fadingOut;
    fadingOut = nullptr;
    collectGarbage();
}

void PreviewEngine::setProgram (std::unique_ptr<Program> p)
{
    if (p != nullptr)
    {
        timelineAtomic.store (p->timelineLength());
        rateAtomic.store (p->sampleRate);
    }
    delete pending.exchange (p.release(), std::memory_order_acq_rel);
}

void PreviewEngine::play()
{
    stopRequested = false;
    playRequested = true;
    playing = true;
}

void PreviewEngine::stop()
{
    playRequested = false;
    stopRequested = true;
    playing = false;
}

void PreviewEngine::rewind()
{
    seek (0);
}

void PreviewEngine::seek (juce::int64 samples)
{
    samples = juce::jmax<juce::int64> (0, samples);
    seekRequest.store (samples);
    positionAtomic.store (samples);
}

void PreviewEngine::setLaneGainDb (int lane, float db)
{
    if (juce::isPositiveAndBelow (lane, 2))
        laneGain[(size_t) lane].store (juce::Decibels::decibelsToGain (juce::jlimit (-60.0f, 12.0f, db), -60.0f));
}

void PreviewEngine::setMasterGainDb (float db)
{
    masterGain.store (juce::Decibels::decibelsToGain (juce::jlimit (-60.0f, 6.0f, db), -60.0f));
}

void PreviewEngine::collectGarbage()
{
    const auto scope = retireFifo.read (retireFifo.getNumReady());
    for (int i = 0; i < scope.blockSize1; ++i) { delete retired[(size_t) (scope.startIndex1 + i)]; retired[(size_t) (scope.startIndex1 + i)] = nullptr; }
    for (int i = 0; i < scope.blockSize2; ++i) { delete retired[(size_t) (scope.startIndex2 + i)]; retired[(size_t) (scope.startIndex2 + i)] = nullptr; }
}

bool PreviewEngine::laneAudible (int lane) const noexcept
{
    return laneTarget (lane) > 0.0f;
}

float PreviewEngine::laneTarget (int lane) const noexcept
{
    const auto m = (Mode) mode.load();
    if (m == Mode::Context && lane == outputLane) return 0.0f;
    if (m == Mode::Output && lane == contextLane) return 0.0f;
    if (mute[(size_t) lane].load()) return 0.0f;
    const bool anySolo = solo[0].load() || solo[1].load();
    if (anySolo && ! solo[(size_t) lane].load()) return 0.0f;
    return laneGain[(size_t) lane].load();
}

void PreviewEngine::prepare (double sampleRate, int maxBlockSize)
{
    hostRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlock = juce::jmax (16, maxBlockSize);
    for (auto* b : { &ctxBuf, &outBuf, &dryBuf, &oldBuf })
    {
        b->setSize (2, maxBlock);
        b->clear();
    }
    fadeSamples = juce::jmax (32, (int) (0.006 * hostRate));
    for (auto& s : laneSmooth)
        s.reset (hostRate, 0.02);
    compareSmooth.reset (hostRate, 0.015);
    masterSmooth.reset (hostRate, 0.02);
}

void PreviewEngine::retire (Program* p) noexcept
{
    if (p == nullptr)
        return;
    const auto scope = retireFifo.write (1);
    if (scope.blockSize1 > 0)      retired[(size_t) scope.startIndex1] = p;
    else if (scope.blockSize2 > 0) retired[(size_t) scope.startIndex2] = p;
}

void PreviewEngine::swapProgramIfPending() noexcept
{
    // Wait until a running crossfade finished and there is room to retire the old program.
    if (pending.load (std::memory_order_acquire) == nullptr || fadingOut != nullptr || retireFifo.getFreeSpace() < 1)
        return;
    auto* incoming = pending.exchange (nullptr, std::memory_order_acq_rel);
    if (incoming == nullptr)
        return;

    if (active && current != nullptr)
    {
        fadingOut = current;       // crossfade old -> new over fadeSamples (retired afterwards)
        swapFadePos = 0;
    }
    else
    {
        retire (current);
    }
    current = incoming;
    const auto len = current->timelineLength();
    pos = len > 0 ? pos % len : 0;
}

void PreviewEngine::readLane (const juce::AudioBuffer<float>& src, juce::int64 start, juce::AudioBuffer<float>& dest, int destStart, int n) const noexcept
{
    const int len = src.getNumSamples();
    if (len <= 0 || src.getNumChannels() <= 0)
    {
        dest.clear (destStart, n);
        return;
    }
    int written = 0;
    auto p = (int) (((start % len) + len) % len);
    while (written < n)
    {
        const int chunk = juce::jmin (n - written, len - p);
        for (int ch = 0; ch < 2; ++ch)
            dest.copyFrom (ch, destStart + written, src, juce::jmin (ch, src.getNumChannels() - 1), p, chunk);
        written += chunk;
        p = 0;
    }
}

void PreviewEngine::readWithSwap (int lane, juce::int64 at, juce::AudioBuffer<float>& dest, int n) noexcept
{
    auto source = [lane] (const Program* p) { return lane == contextLane ? p->context.get() : p->output.get(); };
    if (auto* s = source (current)) readLane (*s, at, dest, 0, n); else dest.clear (0, n);
    if (fadingOut == nullptr)
        return;
    if (auto* s = source (fadingOut)) readLane (*s, at, oldBuf, 0, n); else oldBuf.clear (0, n);
    for (int ch = 0; ch < 2; ++ch)
    {
        auto* d = dest.getWritePointer (ch);
        const auto* o = oldBuf.getReadPointer (ch);
        for (int i = 0; i < n; ++i)
        {
            const float w = juce::jmin (1.0f, (float) (swapFadePos + i) / (float) fadeSamples);
            d[i] = o[i] + w * (d[i] - o[i]);
        }
    }
}

void PreviewEngine::renderAdd (juce::AudioBuffer<float>& out, int numSamples, bool offline, fx::EffectChain& chain) noexcept
{
    swapProgramIfPending();

    if (const auto target = seekRequest.exchange (-1); target >= 0)
    {
        const auto len = current != nullptr ? current->timelineLength() : 0;
        pos = len > 0 ? target % len : 0;
        if (active && ! stopping)
            startRamp = 0.0f;   // fade in from the new position: no click
    }
    if (stopRequested.exchange (false) && active && ! stopping)
    {
        stopping = true;
        stopFadeRemaining = fadeSamples;
    }
    if (playRequested.exchange (false))
    {
        if (current != nullptr && current->timelineLength() > 0)
        {
            const bool wasActive = active && ! stopping;
            active = true;
            stopping = false;
            stopFadeRemaining = 0;
            if (! wasActive)
            {
                startRamp = 0.0f;
                chain.requestReset();
                for (int l = 0; l < 2; ++l)
                    laneSmooth[(size_t) l].setCurrentAndTargetValue (laneTarget (l));
                masterSmooth.setCurrentAndTargetValue (masterGain.load());
                compareSmooth.setCurrentAndTargetValue (compareOriginal.load() ? 1.0f : 0.0f);
            }
        }
        else
        {
            playing.store (false);
        }
    }

    if (! active || current == nullptr || numSamples <= 0 || maxBlock <= 0)
        return;
    if (offline || std::abs (current->sampleRate - hostRate) > 0.5)
        return;   // never in bounces; never at the wrong rate (the session re-renders for the new rate)

    const auto timeline = current->timelineLength();
    if (timeline <= 0)
    {
        active = false;
        playing.store (false);
        return;
    }

    const bool loop = looping.load();
    const int outChans = out.getNumChannels();
    fx::ProcessContext ctx;
    ctx.bpm = tempoBpm.load();

    for (int l = 0; l < 2; ++l)
        laneSmooth[(size_t) l].setTargetValue (laneTarget (l));
    compareSmooth.setTargetValue (compareOriginal.load() ? 1.0f : 0.0f);
    masterSmooth.setTargetValue (masterGain.load());

    int done = 0;
    while (done < numSamples && active)
    {
        // Chunks never cross the end of the timeline, so every lane wraps at the same sample.
        int n = (int) juce::jmin<juce::int64> (juce::jmin (maxBlock, numSamples - done), timeline - pos);
        if (n <= 0)
        {
            active = false;
            playing.store (false);
            pos = 0;
            break;
        }

        const bool hasCtx = current->context != nullptr || (fadingOut != nullptr && fadingOut->context != nullptr);
        // Skip the effect chain entirely while the output lane can't be heard (CPU); reset it
        // when it becomes audible again so old delay/reverb tails don't reappear.
        const bool outAudible = laneSmooth[1].getCurrentValue() > 0.0f || laneSmooth[1].getTargetValue() > 0.0f;
        if (outAudible && ! outputWasAudible)
            chain.requestReset();
        outputWasAudible = outAudible;
        const bool hasOut = outAudible && (current->output != nullptr || (fadingOut != nullptr && fadingOut->output != nullptr));
        const bool needDry = hasOut && (compareOriginal.load() || compareSmooth.isSmoothing() || compareSmooth.getCurrentValue() > 0.0f);

        if (hasCtx)
            readWithSwap (contextLane, pos, ctxBuf, n);
        if (hasOut)
        {
            // Read ahead by the chain latency so the processed result lines up with the context lane.
            readWithSwap (outputLane, pos + chain.realtimeLatency(), outBuf, n);
            juce::AudioBuffer<float> view (outBuf.getArrayOfWritePointers(), 2, 0, n);
            chain.process (view, n, ctx);
            if (needDry)
                readWithSwap (outputLane, pos, dryBuf, n);
        }

        for (int i = 0; i < n; ++i)
        {
            float e = startRamp;
            if (startRamp < 1.0f)
                startRamp = juce::jmin (1.0f, startRamp + 1.0f / (float) fadeSamples);
            if (stopping)
            {
                e *= (float) stopFadeRemaining / (float) fadeSamples;
                if (stopFadeRemaining > 0)
                    --stopFadeRemaining;
            }
            e *= masterSmooth.getNextValue();
            const float gc = laneSmooth[0].getNextValue() * e;
            const float go = laneSmooth[1].getNextValue() * e;
            const float c = compareSmooth.getNextValue();

            float l = 0.0f, r = 0.0f;
            if (hasCtx)
            {
                l += ctxBuf.getSample (0, i) * gc;
                r += ctxBuf.getSample (1, i) * gc;
            }
            if (hasOut)
            {
                float ol = outBuf.getSample (0, i), orr = outBuf.getSample (1, i);
                if (needDry)
                {
                    ol += c * (dryBuf.getSample (0, i) - ol);
                    orr += c * (dryBuf.getSample (1, i) - orr);
                }
                l += ol * go;
                r += orr * go;
            }
            if (outChans == 1)
                out.addSample (0, done + i, 0.5f * (l + r));
            else if (outChans >= 2)
            {
                out.addSample (0, done + i, l);
                out.addSample (1, done + i, r);
            }
        }

        if (fadingOut != nullptr)
        {
            swapFadePos += n;
            if (swapFadePos >= fadeSamples && retireFifo.getFreeSpace() > 0)
            {
                retire (fadingOut);
                fadingOut = nullptr;
            }
        }

        pos += n;
        if (pos >= timeline)
        {
            if (loop)
                pos %= timeline;
            else
            {
                active = false;
                playing.store (false);
                pos = 0;
            }
        }
        done += n;

        if (stopping && stopFadeRemaining <= 0)
        {
            stopping = false;
            active = false;
        }
    }
    positionAtomic.store (pos);
}
} // namespace context
