#include "EqFilterEffects.h"

namespace context::fx
{
namespace
{
    constexpr int updateInterval = 16;
    enum BandType { Bell = 0, LowShelf, HighShelf, LowCut, HighCut, Notch };
}

const juce::StringArray& EqEffect::bandTypes()
{
    static const juce::StringArray t { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch" };
    return t;
}

dsp::Biquad::Type EqEffect::toBiquad (int t)
{
    switch (t)
    {
        case LowShelf:  return dsp::Biquad::Type::LowShelf;
        case HighShelf: return dsp::Biquad::Type::HighShelf;
        case LowCut:    return dsp::Biquad::Type::HighPass;
        case HighCut:   return dsp::Biquad::Type::LowPass;
        case Notch:     return dsp::Biquad::Type::Notch;
        default:        return dsp::Biquad::Type::Bell;
    }
}

EqEffect::EqEffect() : EffectProcessor ("eq", "EQ")
{
    struct Default { bool on; int type; float freq, gain, q; };
    const Default defs[numBands] = { { false, LowCut, 40.0f, 0.0f, 0.71f },
                                     { true, LowShelf, 120.0f, 0.0f, 0.71f },
                                     { true, Bell, 700.0f, 0.0f, 1.0f },
                                     { true, Bell, 3000.0f, 0.0f, 1.0f },
                                     { true, HighShelf, 9000.0f, 0.0f, 0.71f } };
    for (int b = 0; b < numBands; ++b)
    {
        const auto p = "b" + juce::String (b + 1);
        const juce::String n;   // the inspector shows one band at a time (band selector), so names stay short
        auto& band = bands[(size_t) b];
        band.on   = &addParameter (ParameterSpec::onOff (p + "on", "Band " + juce::String (b + 1), defs[b].on, "Enable this band"));
        band.type = &addParameter (ParameterSpec::choice (p + "type", n + "Type", bandTypes(), defs[b].type, "Filter shape"));
        band.freq = &addParameter (ParameterSpec::continuous (p + "freq", n + "Freq", 20.0f, 20000.0f, defs[b].freq, Unit::Hertz, 1000.0f, "Centre / corner frequency"));
        band.gain = &addParameter (ParameterSpec::continuous (p + "gain", n + "Gain", -24.0f, 24.0f, defs[b].gain, Unit::Decibels, 0.0f, "Boost or cut"));
        band.q    = &addParameter (ParameterSpec::continuous (p + "q", n + "Q", 0.1f, 18.0f, defs[b].q, Unit::None, 1.0f, "Bandwidth (higher = narrower); resonance for cuts"));
    }
    output = &addParameter (ParameterSpec::continuous ("output", "Output", -24.0f, 24.0f, 0.0f, Unit::Decibels, 0.0f, "Output level"));
}

bool EqEffect::isParameterRelevant (const juce::String& id) const
{
    if (id.length() < 3 || id[0] != 'b' || ! juce::CharacterFunctions::isDigit (id[1]))
        return true;
    const int b = id[1] - '1';
    if (! juce::isPositiveAndBelow (b, numBands))
        return true;
    const auto& band = bands[(size_t) b];
    const auto suffix = id.substring (2);
    if (suffix == "on")
        return true;
    if (! band.on->getBool())
        return false;
    if (suffix == "gain")
    {
        const int t = band.type->getIndex();
        return t == Bell || t == LowShelf || t == HighShelf;
    }
    return true;
}

void EqEffect::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    reset();
}

void EqEffect::updateBand (Band& b, bool snap) noexcept
{
    const int type = b.type->getIndex();
    b.active = b.on->getBool();
    if (snap)
    {
        b.freqS.snap (b.freq->get());
        b.gainS.snap (b.gain->get());
        b.qS.snap (b.q->get());
    }
    if (type != b.lastType)
    {
        b.lastType = type;
        b.filter.reset();
    }
    b.filter.set (toBiquad (type), b.freqS.current(), b.qS.current(), b.gainS.current(), sampleRate);
}

void EqEffect::reset() noexcept
{
    for (auto& b : bands)
    {
        b.freqS.reset (sampleRate, 0.03, b.freq->get());
        b.gainS.reset (sampleRate, 0.03, b.gain->get());
        b.qS.reset (sampleRate, 0.03, b.q->get());
        b.lastType = -1;
        updateBand (b, true);
        b.filter.reset();
    }
    outS.reset (sampleRate, 0.02, dsp::dbToGain (output->get()));
}

void EqEffect::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext&) noexcept
{
    for (auto& b : bands)
    {
        b.freqS.setTarget (b.freq->get());
        b.gainS.setTarget (b.gain->get());
        b.qS.setTarget (b.q->get());
        if (b.type->getIndex() != b.lastType || b.on->getBool() != b.active)
            updateBand (b, false);
    }
    outS.setTarget (dsp::dbToGain (output->get()));

    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    for (int start = 0; start < n; start += updateInterval)
    {
        const int len = juce::jmin (updateInterval, n - start);
        for (auto& b : bands)
        {
            if (! b.active)
                continue;
            if (b.freqS.isSmoothing() || b.gainS.isSmoothing() || b.qS.isSmoothing())
            {
                b.freqS.skip (len);
                b.gainS.skip (len);
                b.qS.skip (len);
                b.filter.set (toBiquad (b.lastType), b.freqS.current(), b.qS.current(), b.gainS.current(), sampleRate);
            }
            else if (std::abs (b.freqS.current() - b.freq->get()) > 1.0e-3f) // first block after a jump
            {
                b.filter.set (toBiquad (b.lastType), b.freqS.current(), b.qS.current(), b.gainS.current(), sampleRate);
            }
            for (int i = start; i < start + len; ++i)
            {
                L[i] = b.filter.process (L[i], 0);
                R[i] = b.filter.process (R[i], 1);
            }
        }
        for (int i = start; i < start + len; ++i)
        {
            const float g = outS.next();
            L[i] *= g;
            R[i] *= g;
        }
    }
}

float EqEffect::responseDb (float freq) const
{
    double mag = 1.0;
    const double sr = sampleRate > 0 ? sampleRate : 48000.0;
    for (auto& b : bands)
    {
        if (! b.on->getBool())
            continue;
        dsp::Biquad tmp;
        tmp.set (toBiquad (b.type->getIndex()), b.freq->get(), b.q->get(), b.gain->get(), sr);
        mag *= tmp.magnitude (freq, sr);
    }
    return dsp::gainToDb ((float) mag) + output->get();
}

// ---------------------------------------------------------------------------

FilterEffect::FilterEffect()
    : EffectProcessor ("filter", "Filter"),
      mode (addParameter (ParameterSpec::choice ("mode", "Mode", { "Low Pass", "High Pass", "Band Pass", "Notch" }, 0, "Filter type"))),
      cutoff (addParameter (ParameterSpec::continuous ("cutoff", "Cutoff", 20.0f, 20000.0f, 8000.0f, Unit::Hertz, 1000.0f, "Cutoff frequency"))),
      resonance (addParameter (ParameterSpec::continuous ("resonance", "Resonance", 0.0f, 100.0f, 10.0f, Unit::Percent, 0.0f, "Emphasis at the cutoff"))),
      slope (addParameter (ParameterSpec::choice ("slope", "Slope", { "12 dB/oct", "24 dB/oct" }, 0, "Filter steepness"))),
      mix (addParameter (ParameterSpec::continuous ("mix", "Mix", 0.0f, 100.0f, 100.0f, Unit::Percent, 0.0f, "Dry/wet")))
{
}

void FilterEffect::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    reset();
}

void FilterEffect::reset() noexcept
{
    cutoffS.reset (sampleRate, 0.03, cutoff.get());
    resS.reset (sampleRate, 0.03, resonance.get());
    mixS.reset (sampleRate, 0.02, mix.get() / 100.0f);
    stage1.reset();
    stage2.reset();
}

void FilterEffect::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext&) noexcept
{
    cutoffS.setTarget (cutoff.get());
    resS.setTarget (resonance.get());
    mixS.setTarget (mix.get() / 100.0f);
    const int m = mode.getIndex();
    const bool steep = slope.getIndex() == 1;
    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);

    auto pick = [m] (const dsp::Svf::Out& o, double x, double k)
    {
        switch (m)
        {
            case 1:  return o.high;
            case 2:  return o.band * k;          // unity-peak band-pass
            case 3:  return x - k * o.band;      // notch
            default: return o.low;
        }
    };

    for (int start = 0; start < n; start += updateInterval)
    {
        const int len = juce::jmin (updateInterval, n - start);
        cutoffS.skip (len);
        resS.skip (len);
        // Resonance 0..100% -> Q 0.5..12 (exponential feel); 24 dB uses Butterworth-like staging.
        const double q = 0.5 * std::pow (24.0, resS.current() / 100.0);
        stage1.set (cutoffS.current(), steep ? juce::jmax (0.54, q * 0.54 / 0.707) : q, sampleRate);
        stage2.set (cutoffS.current(), steep ? 1.3066 : q, sampleRate);
        for (int i = start; i < start + len; ++i)
        {
            const float w = mixS.next();
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* d = ch == 0 ? L : R;
                const double x = d[i];
                double y = pick (stage1.process (x, ch), x, stage1.k);
                if (steep)
                    y = pick (stage2.process (y, ch), y, stage2.k);
                d[i] = (float) (x + w * (y - x));
            }
        }
    }
}
} // namespace context::fx
