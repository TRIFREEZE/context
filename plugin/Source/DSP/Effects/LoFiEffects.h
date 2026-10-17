#pragma once

#include "DSP/EffectProcessor.h"
#include "DspHelpers.h"

namespace context::fx
{
/** Bit-depth reduction with optional TPDF dither. */
class BitcrusherEffect final : public EffectProcessor
{
public:
    BitcrusherEffect();
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;

private:
    EffectParameter &bits, &dither, &mix;
    juce::Random rng { 1234 };
    SmoothedParam mixS;
};

/** Sample-and-hold sample-rate reduction (aliasing on purpose), optional pre-filter. */
class DownsamplerEffect final : public EffectProcessor
{
public:
    DownsamplerEffect();
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;

private:
    EffectParameter &rate, &filter, &mix;
    double phase = 1.0;
    float held[2] {};
    dsp::Biquad aa1, aa2;
    float lastRate = -1.0f;
    SmoothedParam mixS;
};
} // namespace context::fx
