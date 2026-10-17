#pragma once

#include "DSP/EffectProcessor.h"
#include "DspHelpers.h"
#include <array>

namespace context::fx
{
/** Algorithmic reverb: input diffusion into an 8-line feedback delay network
    (Householder mixing) with per-line decay (RT60), damping and slow modulation. */
class ReverbEffect final : public EffectProcessor
{
public:
    ReverbEffect();
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;
    double getTailSeconds() const noexcept override { return decay.get() + predelay.get() * 0.001; }

private:
    static constexpr int numLines = 8;
    static constexpr int numDiffusers = 4;
    void updateGeometry() noexcept;

    EffectParameter &size, &decay, &predelay, &damping, &width, &mix;
    std::array<dsp::DelayLine, numLines> lines;
    std::array<double, numLines> lineLength {};
    std::array<float, numLines> lineGain {}, dampState {};
    std::array<dsp::Lfo, numLines> mod;
    std::array<dsp::DelayLine, numDiffusers> diffusers;
    std::array<double, numDiffusers> diffLength {};
    dsp::DelayLine preDelayLine;
    dsp::Biquad inputHp;
    float dampCoeff = 0.3f;
    float lastSize = -1.0f, lastDecay = -1.0f;
    SmoothedParam mixS, widthS, preS;
};

/** Stereo / ping-pong / wide delay with tempo sync and filtered, saturating feedback. */
class DelayEffect final : public EffectProcessor
{
public:
    DelayEffect();
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;
    bool isParameterRelevant (const juce::String&) const override;
    double getTailSeconds() const noexcept override;

    static constexpr double maxDelaySeconds = 4.0;

private:
    EffectParameter &sync, &division, &time, &feedback, &lowCut, &highCut, &mode, &mix;
    dsp::DelayLine line;
    std::array<dsp::Biquad, 2> hp, lp;
    juce::SmoothedValue<double> delayS;
    SmoothedParam fbS, mixS;
    float lastLow = -1.0f, lastHigh = -1.0f;
    double lastBpm = 120.0;
};
} // namespace context::fx
