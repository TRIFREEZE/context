#pragma once

#include "EffectRegistry.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <array>

namespace context::fx
{
/** The processing rack: an ordered list of effects with per-slot bypass.

    Model vs. audio
      The ordered list is a message-thread model guarded by a lock (so the DAW can save
      it from any thread). Every edit publishes an immutable Snapshot — the list of slot
      pointers in their new order — to the audio thread through an atomic pointer.
      The audio thread never locks and never frees: replaced snapshots go to a lock-free
      retire FIFO and are deleted by collectGarbage() on the message thread.
      Slots (effect instance + per-slot audio state) are shared between snapshots, so
      reordering keeps every effect's state (reverb tails, envelopes) intact.

    Bypass
      Toggling bypass crossfades dry/wet over 10 ms (no clicks). A fully bypassed effect
      costs nothing except, for effects with latency, a delay line that keeps the dry
      signal time-aligned so the total chain latency never changes while you A/B.

    Order is the signal flow: processing walks the snapshot front to back. */
class EffectChain
{
public:
    static constexpr int maxEffects = 16;

    explicit EffectChain (const EffectRegistry& = EffectRegistry::builtIn());
    ~EffectChain();

    // ---- model (message thread; reads are also safe from other threads) ----
    int size() const;
    EffectProcessor* effect (int index) const;          // valid until that slot is removed
    juce::String instanceId (int index) const;
    juce::String typeId (int index) const;
    bool isBypassed (int index) const;
    int indexOf (const juce::String& instanceId) const;

    /** Creates an effect from the registry; returns its instance id ("" on failure). */
    juce::String add (const juce::String& typeId, int insertAt = -1);
    bool remove (int index);
    bool move (int from, int to);
    bool setBypassed (int index, bool);
    juce::String duplicate (int index);
    bool resetEffect (int index);
    void clear();

    juce::var toVar() const;
    /** Replaces the chain from saved state. Unknown effect types and parameters are
        skipped (reported in the returned warnings); values are clamped. */
    juce::StringArray fromVar (const juce::var&);
    /** Like fromVar, but when the saved chain has the same effects in the same order (an undo
        of a knob move or a bypass), only parameters and bypass are set: effect state such as
        reverb tails survives and nothing is re-created. */
    void applyState (const juce::var&);

    int latencySamples() const;
    double tailSeconds() const;

    /** A fresh, independent chain with the same effects and settings (offline rendering). */
    std::unique_ptr<EffectChain> clone() const;

    const EffectRegistry& registry() const noexcept { return reg; }

    /** Called on the message thread after every model change. */
    std::function<void()> onChanged;

    // ---- lifecycle (non-realtime: prepareToPlay or offline) ----
    void prepare (double sampleRate, int maxBlockSize);
    bool isPrepared() const noexcept { return preparedRate.load() > 0.0; }
    void collectGarbage();

    // ---- audio thread ----
    void process (juce::AudioBuffer<float>& stereo, int numSamples, const ProcessContext&) noexcept;
    void requestReset() noexcept { resetRequested.store (true); }
    int realtimeLatency() const noexcept { return liveLatency.load (std::memory_order_relaxed); }

private:
    struct Slot;
    struct Snapshot;

    std::shared_ptr<Slot> makeSlot (std::unique_ptr<EffectProcessor>, const juce::String& id);
    void prepareSlot (Slot&) const;
    void publishLocked();
    void changed();
    void processChunk (juce::AudioBuffer<float>&, int start, int n, const ProcessContext&) noexcept;

    const EffectRegistry& reg;

    mutable juce::CriticalSection modelLock;
    std::vector<std::shared_ptr<Slot>> slots;        // model order

    std::atomic<double> preparedRate { 0.0 };
    int preparedBlock = 0;
    juce::AudioBuffer<float> dryScratch, wetScratch;  // audio thread after prepare()

    std::atomic<Snapshot*> pending { nullptr };
    Snapshot* current = nullptr;                      // audio thread
    static constexpr int retireCapacity = 64;
    juce::AbstractFifo retireFifo { retireCapacity };
    std::array<Snapshot*, retireCapacity> retired {};
    std::atomic<bool> resetRequested { false };
    std::atomic<int> liveLatency { 0 };

    JUCE_DECLARE_NON_COPYABLE (EffectChain)
};
} // namespace context::fx
