#pragma once

#include "DSP/EffectProcessor.h"
#include "DspHelpers.h"
#include <array>

namespace context::fx
{
/** Five-band parametric EQ. Every band can be a bell, shelf, cut or notch.
    Coefficients are recomputed on the audio thread (allocation-free) every 16 samples
    while a control moves, so sweeps are smooth. */
class EqEffect final : public EffectProcessor
{
public:
    static constexpr int numBands = 5;
    EqEffect();
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;
    bool isParameterRelevant (const juce::String&) const override;

    /** Total magnitude response in dB at `freq` for the current settings (UI curve). */
    float responseDb (float freq) const;

    static const juce::StringArray& bandTypes();

private:
    struct Band
    {
        EffectParameter *on, *type, *freq, *gain, *q;
        dsp::Biquad filter;
        SmoothedParam freqS, gainS, qS;
        int lastType = -1;
        bool active = false;
    };
    static dsp::Biquad::Type toBiquad (int typeIndex);
    void updateBand (Band&, bool snap) noexcept;

    std::array<Band, numBands> bands;
    EffectParameter* output = nullptr;
    SmoothedParam outS;
};

/** Resonant multimode filter (zero-delay-feedback SVF), 12 or 24 dB/oct. */
class FilterEffect final : public EffectProcessor
{
public:
    FilterEffect();
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;

private:
    EffectParameter &mode, &cutoff, &resonance, &slope, &mix;
    dsp::Svf stage1, stage2;
    SmoothedParam cutoffS, resS, mixS;
};
} // namespace context::fx
