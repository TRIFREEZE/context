#pragma once

#include "DSP/EffectProcessor.h"
#include "DspHelpers.h"
#include <juce_dsp/juce_dsp.h>

namespace context::fx
{
/** Shared base: oversampled waveshaping with a latency-aligned dry path for Mix. */
class OversampledShaper : public EffectProcessor
{
public:
    using EffectProcessor::EffectProcessor;
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override { return latency; }

protected:
    /** Oversampling factor as a power of two (1 = 2x, 2 = 4x). */
    virtual int oversamplingLog2() const noexcept { return 1; }
    /** Called once per block before shaping (read parameters here). */
    virtual void beginBlock() noexcept {}
    /** Shapes one oversampled sample of channel ch. */
    virtual float shape (float x, int ch) noexcept = 0;
    /** Post-processing at the base rate (tone, output), on the wet signal. */
    virtual void postProcess (juce::AudioBuffer<float>&, int numSamples) noexcept {}
    virtual float mixAmount() const noexcept { return 1.0f; }

    double osRate = 88200.0;

private:
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    dsp::DelayLine dryDelay;
    juce::AudioBuffer<float> dry;
    int latency = 0;
    SmoothedParam mixS;
};

/** Warm, musical saturation: tape, tube and soft-clip characters, tilt tone. */
class SaturationEffect final : public OversampledShaper
{
public:
    SaturationEffect();
    void reset() noexcept override;

protected:
    void beginBlock() noexcept override;
    float shape (float x, int ch) noexcept override;
    void postProcess (juce::AudioBuffer<float>&, int numSamples) noexcept override;
    float mixAmount() const noexcept override { return mix.get() / 100.0f; }

private:
    EffectParameter &drive, &character, &tone, &output, &mix;
    float driveGain = 1.0f, comp = 1.0f;
    int mode = 0;
    dsp::Biquad lowTilt, highTilt;
    float lastTone = 1.0e9f;
    SmoothedParam outS, driveS;
};

/** Aggressive distortion: hard clip, wavefold, fuzz and rectifier, with a tone filter. */
class DistortionEffect final : public OversampledShaper
{
public:
    DistortionEffect();
    void reset() noexcept override;

protected:
    void beginBlock() noexcept override;
    float shape (float x, int ch) noexcept override;
    void postProcess (juce::AudioBuffer<float>&, int numSamples) noexcept override;
    float mixAmount() const noexcept override { return mix.get() / 100.0f; }

private:
    EffectParameter &drive, &mode, &tone, &output, &mix;
    float driveGain = 1.0f, comp = 1.0f;
    int shapeMode = 0;
    dsp::Biquad toneFilter;
    dsp::DcBlocker dc;
    float lastTone = -1.0f;
    SmoothedParam outS, driveS;
};

/** Transparent soft clipper (4x oversampled): linear below the knee, smooth to the ceiling. */
class SoftClipperEffect final : public OversampledShaper
{
public:
    SoftClipperEffect();
    void reset() noexcept override;

protected:
    int oversamplingLog2() const noexcept override { return 2; }
    void beginBlock() noexcept override;
    float shape (float x, int ch) noexcept override;
    void postProcess (juce::AudioBuffer<float>&, int numSamples) noexcept override;

private:
    EffectParameter &inputGain, &ceiling, &softness, &output;
    float gainIn = 1.0f, ceil = 1.0f, knee = 0.5f;
    SmoothedParam outS;
};
} // namespace context::fx
