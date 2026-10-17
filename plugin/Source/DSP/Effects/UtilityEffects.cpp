#include "UtilityEffects.h"

namespace context::fx
{
UtilityEffect::UtilityEffect()
    : EffectProcessor ("utility", "Utility"),
      gain (addParameter (ParameterSpec::continuous ("gain", "Gain", -36.0f, 24.0f, 0.0f, Unit::Decibels, 0.0f, "Output level"))),
      balance (addParameter (ParameterSpec::continuous ("balance", "Balance", -100.0f, 100.0f, 0.0f, Unit::Percent, 0.0f, "Left/right balance"))),
      invertL (addParameter (ParameterSpec::onOff ("invertL", "Invert L", false, "Flip the polarity of the left channel"))),
      invertR (addParameter (ParameterSpec::onOff ("invertR", "Invert R", false, "Flip the polarity of the right channel"))),
      mono (addParameter (ParameterSpec::onOff ("mono", "Mono", false, "Sum to mono"))),
      dc (addParameter (ParameterSpec::onOff ("dc", "DC Filter", false, "Remove DC offset (10 Hz high-pass)")))
{
}

void UtilityEffect::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    dcBlock.prepare (sr);
    reset();
}

void UtilityEffect::reset() noexcept
{
    gainS.reset (sampleRate, 0.02, dsp::dbToGain (gain.get()));
    const float b = balance.get() / 100.0f;
    leftS.reset (sampleRate, 0.02, juce::jmin (1.0f, 1.0f - b));
    rightS.reset (sampleRate, 0.02, juce::jmin (1.0f, 1.0f + b));
    dcBlock.reset();
}

void UtilityEffect::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext&) noexcept
{
    gainS.setTarget (dsp::dbToGain (gain.get()));
    const float b = balance.get() / 100.0f;
    leftS.setTarget (juce::jmin (1.0f, 1.0f - b));
    rightS.setTarget (juce::jmin (1.0f, 1.0f + b));
    const float pl = invertL.getBool() ? -1.0f : 1.0f, pr = invertR.getBool() ? -1.0f : 1.0f;
    const bool toMono = mono.getBool(), useDc = dc.getBool();
    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        float l = L[i] * pl, r = R[i] * pr;
        if (useDc) { l = dcBlock.process (l, 0); r = dcBlock.process (r, 1); }
        if (toMono) { const float m = 0.5f * (l + r); l = r = m; }
        const float g = gainS.next();
        L[i] = l * g * leftS.next();
        R[i] = r * g * rightS.next();
    }
}

// ---------------------------------------------------------------------------

StereoEffect::StereoEffect()
    : EffectProcessor ("stereo", "Stereo"),
      width (addParameter (ParameterSpec::continuous ("width", "Width", 0.0f, 200.0f, 100.0f, Unit::Percent, 0.0f, "0% = mono, 100% = unchanged, 200% = extra wide"))),
      bassMono (addParameter (ParameterSpec::onOff ("bassMono", "Bass Mono", false, "Make everything below the crossover mono"))),
      monoFreq (addParameter (ParameterSpec::continuous ("monoFreq", "Crossover", 40.0f, 500.0f, 120.0f, Unit::Hertz, 150.0f, "Frequency below which the low end is made mono"))),
      swap (addParameter (ParameterSpec::onOff ("swap", "Swap L/R", false, "Exchange left and right")))
{
}

bool StereoEffect::isParameterRelevant (const juce::String& id) const
{
    return id != "monoFreq" || bassMono.getBool();
}

void StereoEffect::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    reset();
}

void StereoEffect::reset() noexcept
{
    widthS.reset (sampleRate, 0.03, width.get() / 100.0f);
    sideHp1.reset();
    sideHp2.reset();
    lastFreq = -1.0f;
}

void StereoEffect::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext&) noexcept
{
    widthS.setTarget (width.get() / 100.0f);
    const bool lowMono = bassMono.getBool();
    if (lowMono && monoFreq.get() != lastFreq)
    {
        lastFreq = monoFreq.get();
        // Butterworth 4th order = two biquads with Q 0.541 and 1.307.
        sideHp1.set (dsp::Biquad::Type::HighPass, lastFreq, 0.5412, 0.0, sampleRate);
        sideHp2.set (dsp::Biquad::Type::HighPass, lastFreq, 1.3066, 0.0, sampleRate);
    }
    const bool doSwap = swap.getBool();
    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        const float m = 0.5f * (L[i] + R[i]);
        float s = 0.5f * (L[i] - R[i]);
        if (lowMono)
            s = sideHp2.process (sideHp1.process (s, 0), 0);
        s *= widthS.next();
        float l = m + s, r = m - s;
        if (doSwap) std::swap (l, r);
        L[i] = l;
        R[i] = r;
    }
}
} // namespace context::fx
