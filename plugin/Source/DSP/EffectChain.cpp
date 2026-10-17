#include "EffectChain.h"

namespace context::fx
{
struct EffectChain::Slot
{
    std::shared_ptr<EffectProcessor> fx;
    juce::String instanceId;
    std::atomic<bool> bypassed { false };

    // Audio-thread state (also touched by prepare(), never concurrently with process()).
    float gain = 1.0f;                       // 1 = fully active, 0 = fully bypassed
    int latency = 0;
    juce::AudioBuffer<float> dryRing;        // keeps the dry path aligned for latent effects
    int ringPos = 0;
    double rampPerSample = 1.0 / 441.0;
};

struct EffectChain::Snapshot
{
    std::vector<std::shared_ptr<Slot>> slots;
    int latency = 0;
};

EffectChain::EffectChain (const EffectRegistry& r) : reg (r) {}

EffectChain::~EffectChain()
{
    // The audio thread is gone by now (owner is being destroyed).
    delete pending.exchange (nullptr);
    delete current;
    current = nullptr;
    collectGarbage();
}

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

int EffectChain::size() const
{
    const juce::ScopedLock sl (modelLock);
    return (int) slots.size();
}

EffectProcessor* EffectChain::effect (int index) const
{
    const juce::ScopedLock sl (modelLock);
    return juce::isPositiveAndBelow (index, (int) slots.size()) ? slots[(size_t) index]->fx.get() : nullptr;
}

juce::String EffectChain::instanceId (int index) const
{
    const juce::ScopedLock sl (modelLock);
    return juce::isPositiveAndBelow (index, (int) slots.size()) ? slots[(size_t) index]->instanceId : juce::String();
}

juce::String EffectChain::typeId (int index) const
{
    const juce::ScopedLock sl (modelLock);
    return juce::isPositiveAndBelow (index, (int) slots.size()) ? slots[(size_t) index]->fx->typeId() : juce::String();
}

bool EffectChain::isBypassed (int index) const
{
    const juce::ScopedLock sl (modelLock);
    return juce::isPositiveAndBelow (index, (int) slots.size()) && slots[(size_t) index]->bypassed.load();
}

int EffectChain::indexOf (const juce::String& id) const
{
    const juce::ScopedLock sl (modelLock);
    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i]->instanceId == id)
            return (int) i;
    return -1;
}

std::shared_ptr<EffectChain::Slot> EffectChain::makeSlot (std::unique_ptr<EffectProcessor> fx, const juce::String& id)
{
    auto s = std::make_shared<Slot>();
    s->fx = std::move (fx);
    s->instanceId = id.isNotEmpty() ? id : juce::Uuid().toDashedString();
    if (isPrepared())
        prepareSlot (*s);
    return s;
}

void EffectChain::prepareSlot (Slot& s) const
{
    const double sr = preparedRate.load();
    const int block = juce::jmax (1, preparedBlock);
    s.fx->prepare (sr, block);
    s.fx->reset();
    s.latency = juce::jmax (0, s.fx->getLatencySamples());
    s.dryRing.setSize (2, s.latency + block + 1);
    s.dryRing.clear();
    s.ringPos = 0;
    s.gain = s.bypassed.load() ? 0.0f : 1.0f;
    s.rampPerSample = 1.0 / juce::jmax (1.0, 0.010 * sr);
}

juce::String EffectChain::add (const juce::String& typeId, int insertAt)
{
    auto fx = reg.create (typeId);
    if (fx == nullptr)
        return {};
    auto slot = makeSlot (std::move (fx), {});
    {
        const juce::ScopedLock sl (modelLock);
        if ((int) slots.size() >= maxEffects)
            return {};
        const int at = insertAt < 0 || insertAt > (int) slots.size() ? (int) slots.size() : insertAt;
        slots.insert (slots.begin() + at, slot);
        publishLocked();
    }
    changed();
    return slot->instanceId;
}

bool EffectChain::remove (int index)
{
    {
        const juce::ScopedLock sl (modelLock);
        if (! juce::isPositiveAndBelow (index, (int) slots.size()))
            return false;
        slots.erase (slots.begin() + index);
        publishLocked();
    }
    changed();
    return true;
}

bool EffectChain::move (int from, int to)
{
    {
        const juce::ScopedLock sl (modelLock);
        const int n = (int) slots.size();
        if (! juce::isPositiveAndBelow (from, n) || ! juce::isPositiveAndBelow (to, n) || from == to)
            return false;
        auto s = slots[(size_t) from];
        slots.erase (slots.begin() + from);
        slots.insert (slots.begin() + to, s);
        publishLocked();
    }
    changed();
    return true;
}

bool EffectChain::setBypassed (int index, bool b)
{
    {
        const juce::ScopedLock sl (modelLock);
        if (! juce::isPositiveAndBelow (index, (int) slots.size()))
            return false;
        slots[(size_t) index]->bypassed.store (b);
    }
    changed();
    return true;
}

juce::String EffectChain::duplicate (int index)
{
    juce::String type;
    juce::var params;
    bool bypass = false;
    {
        const juce::ScopedLock sl (modelLock);
        if (! juce::isPositiveAndBelow (index, (int) slots.size()) || (int) slots.size() >= maxEffects)
            return {};
        type = slots[(size_t) index]->fx->typeId();
        params = slots[(size_t) index]->fx->getParameterState();
        bypass = slots[(size_t) index]->bypassed.load();
    }
    auto fx = reg.create (type);
    if (fx == nullptr)
        return {};
    fx->setParameterState (params);
    auto slot = makeSlot (std::move (fx), {});
    slot->bypassed.store (bypass);
    slot->gain = bypass ? 0.0f : 1.0f;
    {
        const juce::ScopedLock sl (modelLock);
        slots.insert (slots.begin() + juce::jmin (index + 1, (int) slots.size()), slot);
        publishLocked();
    }
    changed();
    return slot->instanceId;
}

bool EffectChain::resetEffect (int index)
{
    {
        const juce::ScopedLock sl (modelLock);
        if (! juce::isPositiveAndBelow (index, (int) slots.size()))
            return false;
        slots[(size_t) index]->fx->resetParameters();
    }
    changed();
    return true;
}

void EffectChain::clear()
{
    {
        const juce::ScopedLock sl (modelLock);
        slots.clear();
        publishLocked();
    }
    changed();
}

juce::var EffectChain::toVar() const
{
    const juce::ScopedLock sl (modelLock);
    juce::Array<juce::var> list;
    for (auto& s : slots)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("type", s->fx->typeId());
        o->setProperty ("id", s->instanceId);
        o->setProperty ("bypass", s->bypassed.load());
        o->setProperty ("params", s->fx->getParameterState());
        list.add (juce::var (o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("v", 1);
    root->setProperty ("fx", list);
    return juce::var (root);
}

juce::StringArray EffectChain::fromVar (const juce::var& v)
{
    juce::StringArray warnings;
    std::vector<std::shared_ptr<Slot>> fresh;
    if (auto* list = v.getProperty ("fx", {}).getArray())
    {
        for (auto& item : *list)
        {
            if ((int) fresh.size() >= maxEffects)
            {
                warnings.add ("More than " + juce::String (maxEffects) + " effects: extra effects were skipped.");
                break;
            }
            const auto type = item.getProperty ("type", {}).toString();
            auto fx = reg.create (type);
            if (fx == nullptr)
            {
                warnings.add ("Unknown effect '" + type.substring (0, 32) + "' was skipped.");
                continue;
            }
            fx->setParameterState (item.getProperty ("params", {}));
            auto id = item.getProperty ("id", {}).toString().substring (0, 64);
            if (! id.containsOnly ("abcdefABCDEF0123456789-") || id.isEmpty())
                id = {};
            auto slot = makeSlot (std::move (fx), id);
            const bool bypass = (bool) item.getProperty ("bypass", false);
            slot->bypassed.store (bypass);
            slot->gain = bypass ? 0.0f : 1.0f;
            fresh.push_back (slot);
        }
    }
    {
        const juce::ScopedLock sl (modelLock);
        slots = std::move (fresh);
        publishLocked();
    }
    changed();
    return warnings;
}

void EffectChain::applyState (const juce::var& v)
{
    auto* list = v.getProperty ("fx", {}).getArray();
    bool sameShape = list != nullptr;
    if (sameShape)
    {
        const juce::ScopedLock sl (modelLock);
        sameShape = list->size() == (int) slots.size();
        for (int i = 0; sameShape && i < list->size(); ++i)
            sameShape = (*list)[i].getProperty ("type", {}).toString() == slots[(size_t) i]->fx->typeId()
                     && (*list)[i].getProperty ("id", {}).toString() == slots[(size_t) i]->instanceId;
    }
    if (! sameShape)
    {
        fromVar (v);
        return;
    }
    {
        const juce::ScopedLock sl (modelLock);
        for (int i = 0; i < list->size(); ++i)
        {
            auto& slot = *slots[(size_t) i];
            slot.fx->setParameterState ((*list)[i].getProperty ("params", {}));
            slot.bypassed.store ((bool) (*list)[i].getProperty ("bypass", false));
        }
    }
    changed();
}

int EffectChain::latencySamples() const
{
    const juce::ScopedLock sl (modelLock);
    int total = 0;
    for (auto& s : slots)
        total += juce::jmax (0, s->fx->getLatencySamples());
    return total;
}

double EffectChain::tailSeconds() const
{
    const juce::ScopedLock sl (modelLock);
    double t = 0.0;
    for (auto& s : slots)
        if (! s->bypassed.load())
            t += s->fx->getTailSeconds();
    return t;
}

std::unique_ptr<EffectChain> EffectChain::clone() const
{
    auto c = std::make_unique<EffectChain> (reg);
    c->fromVar (toVar());
    return c;
}

void EffectChain::changed()
{
    collectGarbage();
    if (onChanged)
        onChanged();
}

void EffectChain::publishLocked()
{
    auto snap = std::make_unique<Snapshot>();
    snap->slots = slots;
    for (auto& s : slots)
        snap->latency += s->latency;
    delete pending.exchange (snap.release(), std::memory_order_acq_rel);   // unconsumed snapshots were never seen by audio
}

void EffectChain::prepare (double sampleRate, int maxBlockSize)
{
    const juce::ScopedLock sl (modelLock);
    preparedBlock = juce::jmax (16, maxBlockSize);
    preparedRate.store (sampleRate > 0.0 ? sampleRate : 44100.0);
    dryScratch.setSize (2, preparedBlock);
    wetScratch.setSize (2, preparedBlock);
    for (auto& s : slots)
        prepareSlot (*s);
    publishLocked();   // latencies may have changed with the sample rate
}

void EffectChain::collectGarbage()
{
    const auto scope = retireFifo.read (retireFifo.getNumReady());
    for (int i = 0; i < scope.blockSize1; ++i) { delete retired[(size_t) (scope.startIndex1 + i)]; retired[(size_t) (scope.startIndex1 + i)] = nullptr; }
    for (int i = 0; i < scope.blockSize2; ++i) { delete retired[(size_t) (scope.startIndex2 + i)]; retired[(size_t) (scope.startIndex2 + i)] = nullptr; }
}

// ---------------------------------------------------------------------------
// Audio thread
// ---------------------------------------------------------------------------

void EffectChain::process (juce::AudioBuffer<float>& buffer, int numSamples, const ProcessContext& ctx) noexcept
{
    if (pending.load (std::memory_order_acquire) != nullptr && retireFifo.getFreeSpace() > 0)
    {
        if (auto* incoming = pending.exchange (nullptr, std::memory_order_acq_rel))
        {
            if (current != nullptr)
            {
                const auto scope = retireFifo.write (1);
                if (scope.blockSize1 > 0)      retired[(size_t) scope.startIndex1] = current;
                else if (scope.blockSize2 > 0) retired[(size_t) scope.startIndex2] = current;
            }
            current = incoming;
            liveLatency.store (current->latency, std::memory_order_relaxed);
        }
    }
    if (current == nullptr || ! isPrepared() || numSamples <= 0 || buffer.getNumChannels() < 2)
        return;

    if (resetRequested.exchange (false))
        for (auto& s : current->slots)
        {
            s->fx->reset();
            s->dryRing.clear();
            s->ringPos = 0;
            s->gain = s->bypassed.load() ? 0.0f : 1.0f;
        }

    for (int start = 0; start < numSamples; start += preparedBlock)
        processChunk (buffer, start, juce::jmin (preparedBlock, numSamples - start), ctx);
}

void EffectChain::processChunk (juce::AudioBuffer<float>& buffer, int start, int n, const ProcessContext& ctx) noexcept
{
    // Effects always see a buffer that starts at sample 0 with exactly n samples.
    juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), 2, start, n);

    for (auto& slotPtr : current->slots)
    {
        auto& s = *slotPtr;
        const float target = s.bypassed.load (std::memory_order_relaxed) ? 0.0f : 1.0f;

        // Dry path (delayed by the effect's latency so bypass never shifts timing).
        const bool needDry = s.latency > 0 || s.gain != target || target == 0.0f;
        if (needDry)
        {
            if (s.latency > 0)
            {
                const int ringSize = s.dryRing.getNumSamples();
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* ring = s.dryRing.getWritePointer (ch);
                    const auto* in = view.getReadPointer (ch);
                    auto* dry = dryScratch.getWritePointer (ch);
                    int w = s.ringPos;
                    for (int i = 0; i < n; ++i)
                    {
                        ring[w] = in[i];
                        int r = w - s.latency;
                        if (r < 0) r += ringSize;
                        dry[i] = ring[r];
                        if (++w == ringSize) w = 0;
                    }
                }
                s.ringPos = (s.ringPos + n) % ringSize;
            }
            else
            {
                for (int ch = 0; ch < 2; ++ch)
                    dryScratch.copyFrom (ch, 0, view, ch, 0, n);
            }
        }

        if (s.gain == 0.0f && target == 0.0f)
        {
            if (s.latency > 0)
                for (int ch = 0; ch < 2; ++ch)
                    view.copyFrom (ch, 0, dryScratch, ch, 0, n);
            continue;   // fully bypassed: no DSP cost
        }

        if (s.gain == 0.0f && target == 1.0f)
            s.fx->reset();   // waking up: don't replay stale tails from before the bypass

        s.fx->process (view, n, ctx);

        if (s.gain == target)
            continue;   // fully active, no crossfade

        float g = s.gain;
        const float step = (float) s.rampPerSample * (target > g ? 1.0f : -1.0f);
        for (int i = 0; i < n; ++i)
        {
            g = target > s.gain ? juce::jmin (target, g + step) : juce::jmax (target, g + step);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* w = view.getWritePointer (ch);
                const float d = dryScratch.getSample (ch, i);
                w[i] = d + g * (w[i] - d);
            }
        }
        s.gain = g;
    }
}
} // namespace context::fx
