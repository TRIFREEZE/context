#include "LoFiEffects.h"

namespace context::fx
{
BitcrusherEffect::BitcrusherEffect()
    : EffectProcessor ("bitcrush", "Bitcrusher"),
      bits (addParameter (ParameterSpec::continuous ("bits", "Bits", 2.0f, 16.0f, 8.0f, Unit::Bits, 8.0f, "Resolution: fewer bits = grittier"))),
      dither (addParameter (ParameterSpec::onOff ("dither", "Dither", false, "Add noise to smooth the steps"))),
      mix (addParameter (ParameterSpec::continuous ("mix", "Mix", 0.0f, 100.0f, 100.0f, Unit::Percent, 0.0f, "Dry/wet")))
{
}

void BitcrusherEffect::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    reset();
}

void BitcrusherEffect::reset() noexcept
{
    mixS.reset (sampleRate, 0.02, mix.get() / 100.0f);
}

void BitcrusherEffect::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext&) noexcept
{
    mixS.setTarget (mix.get() / 100.0f);
    // Fractional bit depths are allowed (smooth sweeps): levels = 2^bits.
    const float levels = std::pow (2.0f, bits.get() - 1.0f);
    const float step = 1.0f / levels;
    const bool useDither = dither.getBool();
    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        const float m = mixS.next();
        for (auto* d : { L, R })
        {
            float x = d[i];
            if (useDither)
                x += (rng.nextFloat() + rng.nextFloat() - 1.0f) * step;
            const float q = std::round (x * levels) / levels;
            d[i] += m * (q - d[i]);
        }
    }
}

// ---------------------------------------------------------------------------

DownsamplerEffect::DownsamplerEffect()
    : EffectProcessor ("downsample", "Downsampler"),
      rate (addParameter (ParameterSpec::continuous ("rate", "Rate", 200.0f, 44100.0f, 8000.0f, Unit::Hertz, 4000.0f, "Target sample rate"))),
      filter (addParameter (ParameterSpec::onOff ("filter", "Filter", false, "Low-pass before reducing (less harsh aliasing)"))),
      mix (addParameter (ParameterSpec::continuous ("mix", "Mix", 0.0f, 100.0f, 100.0f, Unit::Percent, 0.0f, "Dry/wet")))
{
}

void DownsamplerEffect::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    reset();
}

void DownsamplerEffect::reset() noexcept
{
    phase = 1.0;
    held[0] = held[1] = 0.0f;
    aa1.reset();
    aa2.reset();
    lastRate = -1.0f;
    mixS.reset (sampleRate, 0.02, mix.get() / 100.0f);
}

void DownsamplerEffect::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext&) noexcept
{
    mixS.setTarget (mix.get() / 100.0f);
    const float target = juce::jmin (rate.get(), (float) sampleRate);
    const bool useFilter = filter.getBool();
    if (useFilter && target != lastRate)
    {
        lastRate = target;
        aa1.set (dsp::Biquad::Type::LowPass, target * 0.45, 0.5412, 0.0, sampleRate);
        aa2.set (dsp::Biquad::Type::LowPass, target * 0.45, 1.3066, 0.0, sampleRate);
    }
    const double inc = target / sampleRate;
    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        float l = L[i], r = R[i];
        if (useFilter)
        {
            l = aa2.process (aa1.process (l, 0), 0);
            r = aa2.process (aa1.process (r, 1), 1);
        }
        phase += inc;
        if (phase >= 1.0)
        {
            phase -= std::floor (phase);
            held[0] = l;
            held[1] = r;
        }
        const float m = mixS.next();
        L[i] += m * (held[0] - L[i]);
        R[i] += m * (held[1] - R[i]);
    }
}
} // namespace context::fx
