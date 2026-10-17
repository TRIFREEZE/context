#pragma once

#include "DSP/EffectProcessor.h"
#include "DspHelpers.h"
#include <array>

namespace context::fx
{
/** Two-voice stereo chorus. */
class ChorusEffect final : public EffectProcessor
{
public:
    ChorusEffect();
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;

private:
    EffectParameter &rate, &depth, &delayMs, &mix;
    dsp::DelayLine line;
    dsp::Lfo lfo;
    SmoothedParam depthS, delayS, mixS;
};

/** Short modulated delay with feedback. */
class FlangerEffect final : public EffectProcessor
{
public:
    FlangerEffect();
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;

private:
    EffectParameter &rate, &depth, &delayMs, &feedback, &mix;
    dsp::DelayLine line;
    dsp::Lfo lfo;
    float fbState[2] {};
    SmoothedParam depthS, delayS, fbS, mixS;
};

/** Swept all-pass phaser (4-12 stages) with feedback. */
class PhaserEffect final : public EffectProcessor
{
public:
    PhaserEffect();
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;

private:
    static constexpr int maxStages = 12;
    EffectParameter &rate, &depth, &centre, &feedback, &stages, &mix;
    dsp::Lfo lfo;
    std::array<std::array<float, maxStages>, 2> z {};
    float coeff[2] {};
    float fbState[2] {};
    SmoothedParam mixS, fbS;
};

/** Shared LFO-rate parameters (free Hz or tempo-synced). */
class LfoEffectBase : public EffectProcessor
{
public:
    LfoEffectBase (juce::String id, juce::String name, float defaultRate);
    bool isParameterRelevant (const juce::String&) const override;

protected:
    double cyclesPerSample (const ProcessContext&) const noexcept;
    EffectParameter &rate, &sync, &division, &depth, &shape;
    dsp::Lfo lfo;
};

/** Amplitude modulation. */
class TremoloEffect final : public LfoEffectBase
{
public:
    TremoloEffect();
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;

private:
    SmoothedParam depthS;
};

/** Equal-power left/right panning modulation. */
class AutoPanEffect final : public LfoEffectBase
{
public:
    AutoPanEffect();
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;

private:
    SmoothedParam depthS;
};
} // namespace context::fx
