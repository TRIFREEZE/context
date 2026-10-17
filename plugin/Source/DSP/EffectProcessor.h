#pragma once

#include "EffectParameter.h"
#include <memory>
#include <vector>

namespace context::fx
{
/** Musical context handed to effects every block (tempo-synced delay, LFOs). */
struct ProcessContext
{
    double bpm = 120.0;
};

/** Base class for every effect in the processing rack.

    Threading contract
      prepare()              non-realtime (prepareToPlay, offline renderer, or before an
                             effect is published to the audio thread). May allocate.
      reset(), process()     realtime-safe: no allocation, no locks, no I/O.
      parameters             atomics; the UI writes, DSP reads every block and smooths.

    process() always receives a STEREO buffer with numSamples <= the prepared block size.
    Effects that change the signal timing report it through getLatencySamples(); the
    chain compensates, so effects never need to know about the preview engine. */
class EffectProcessor
{
public:
    EffectProcessor (juce::String typeId, juce::String displayName);
    virtual ~EffectProcessor() = default;

    const juce::String& typeId() const noexcept       { return type; }
    const juce::String& displayName() const noexcept  { return name; }

    const std::vector<std::unique_ptr<EffectParameter>>& parameters() const noexcept { return params; }
    EffectParameter* parameter (const juce::String& paramId) const noexcept;

    virtual void prepare (double sampleRate, int maxBlockSize) = 0;
    virtual void reset() noexcept = 0;
    virtual void process (juce::AudioBuffer<float>& stereo, int numSamples, const ProcessContext&) noexcept = 0;

    virtual int getLatencySamples() const noexcept     { return 0; }
    virtual double getTailSeconds() const noexcept     { return 0.0; }

    /** False when a parameter currently has no effect (e.g. the Gain of a low-cut band,
        or the free Time of a tempo-synced delay). The UI shows such controls disabled,
        so nothing on screen pretends to do something it doesn't. */
    virtual bool isParameterRelevant (const juce::String& /*paramId*/) const { return true; }

    /** Gain reduction (dB, <= 0) for dynamics effects, for the inspector meter. */
    virtual std::optional<float> getGainReductionDb() const noexcept { return std::nullopt; }

    /** Parameter values as { id: value }. */
    juce::var getParameterState() const;
    /** Sets known parameters (clamped), ignores unknown ones, leaves missing at their current value. */
    void setParameterState (const juce::var&);
    void resetParameters() noexcept;

protected:
    EffectParameter& addParameter (ParameterSpec);

    double sampleRate = 44100.0;
    int blockSize = 512;

private:
    juce::String type, name;
    std::vector<std::unique_ptr<EffectParameter>> params;

    JUCE_DECLARE_NON_COPYABLE (EffectProcessor)
};

/** Linear ramp toward a target, for click-free parameter changes (block-rate input,
    sample-rate output). */
class SmoothedParam
{
public:
    void reset (double sampleRate, double rampSeconds, float initial) noexcept
    {
        value.reset (sampleRate, rampSeconds);
        value.setCurrentAndTargetValue (initial);
    }
    void setTarget (float v) noexcept   { value.setTargetValue (v); }
    float next() noexcept               { return value.getNextValue(); }
    float current() const noexcept     { return value.getCurrentValue(); }
    bool isSmoothing() const noexcept   { return value.isSmoothing(); }
    void skip (int n) noexcept          { value.skip (n); }
    void snap (float v) noexcept        { value.setCurrentAndTargetValue (v); }

private:
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> value;
};
} // namespace context::fx
