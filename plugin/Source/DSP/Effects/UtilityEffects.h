#pragma once

#include "DSP/EffectProcessor.h"
#include "DspHelpers.h"

namespace context::fx
{
/** Gain, balance, polarity, mono and DC removal. */
class UtilityEffect final : public EffectProcessor
{
public:
    UtilityEffect();
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;

private:
    EffectParameter &gain, &balance, &invertL, &invertR, &mono, &dc;
    SmoothedParam gainS, leftS, rightS;
    dsp::DcBlocker dcBlock;
};

/** Stereo width (mid/side), mono low end below a crossover, swap channels. */
class StereoEffect final : public EffectProcessor
{
public:
    StereoEffect();
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;
    bool isParameterRelevant (const juce::String&) const override;

private:
    EffectParameter &width, &bassMono, &monoFreq, &swap;
    SmoothedParam widthS;
    dsp::Biquad sideHp1, sideHp2;     // 24 dB/oct high-pass on the side signal
    float lastFreq = -1.0f;
};
} // namespace context::fx
