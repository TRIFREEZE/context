#include "ModulationEffects.h"

namespace context::fx
{
// ===========================================================================
// Chorus
// ===========================================================================
ChorusEffect::ChorusEffect()
    : EffectProcessor ("chorus", "Chorus"),
      rate (addParameter (ParameterSpec::continuous ("rate", "Rate", 0.05f, 5.0f, 0.6f, Unit::Hertz, 0.8f, "Modulation speed"))),
      depth (addParameter (ParameterSpec::continuous ("depth", "Depth", 0.0f, 100.0f, 40.0f, Unit::Percent, 0.0f, "Modulation amount"))),
      delayMs (addParameter (ParameterSpec::continuous ("delay", "Delay", 5.0f, 30.0f, 12.0f, Unit::Milliseconds, 0.0f, "Base delay of the voices"))),
      mix (addParameter (ParameterSpec::continuous ("mix", "Mix", 0.0f, 100.0f, 50.0f, Unit::Percent, 0.0f, "Dry/wet")))
{
}

void ChorusEffect::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    line.prepare (2, (int) (0.05 * sr));
    reset();
}

void ChorusEffect::reset() noexcept
{
    line.reset();
    lfo.reset();
    depthS.reset (sampleRate, 0.03, depth.get() / 100.0f);
    delayS.reset (sampleRate, 0.05, delayMs.get());
    mixS.reset (sampleRate, 0.02, mix.get() / 100.0f);
}

void ChorusEffect::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext&) noexcept
{
    depthS.setTarget (depth.get() / 100.0f);
    delayS.setTarget (delayMs.get());
    mixS.setTarget (mix.get() / 100.0f);
    const double inc = rate.get() / sampleRate;
    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        line.push (0, L[i]);
        line.push (1, R[i]);
        const double base = delayS.next() * 0.001 * sampleRate;
        const double swing = depthS.next() * juce::jmin (base * 0.8, 0.006 * sampleRate);
        // Two voices per side with quadrature LFO offsets -> wide, smooth ensemble.
        const double p = lfo.phase;
        auto voice = [&] (int ch, double offset)
        {
            const double v = std::sin (2.0 * dsp::pi * (p + offset));
            return line.readCubic (ch, base + swing * v);
        };
        const float wl = 0.5f * (voice (0, 0.0) + voice (0, 0.5));
        const float wr = 0.5f * (voice (1, 0.25) + voice (1, 0.75));
        lfo.next (inc, dsp::Lfo::Shape::Sine);
        line.advance();
        const float m = mixS.next();
        L[i] += m * (wl - L[i]);
        R[i] += m * (wr - R[i]);
    }
}

// ===========================================================================
// Flanger
// ===========================================================================
FlangerEffect::FlangerEffect()
    : EffectProcessor ("flanger", "Flanger"),
      rate (addParameter (ParameterSpec::continuous ("rate", "Rate", 0.02f, 5.0f, 0.25f, Unit::Hertz, 0.5f, "Sweep speed"))),
      depth (addParameter (ParameterSpec::continuous ("depth", "Depth", 0.0f, 100.0f, 70.0f, Unit::Percent, 0.0f, "Sweep amount"))),
      delayMs (addParameter (ParameterSpec::continuous ("delay", "Delay", 0.1f, 10.0f, 1.5f, Unit::Milliseconds, 2.0f, "Centre of the sweep"))),
      feedback (addParameter (ParameterSpec::continuous ("feedback", "Feedback", -95.0f, 95.0f, 50.0f, Unit::Percent, 0.0f, "Resonance; negative values give a hollow tone"))),
      mix (addParameter (ParameterSpec::continuous ("mix", "Mix", 0.0f, 100.0f, 50.0f, Unit::Percent, 0.0f, "Dry/wet")))
{
}

void FlangerEffect::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    line.prepare (2, (int) (0.025 * sr));
    reset();
}

void FlangerEffect::reset() noexcept
{
    line.reset();
    lfo.reset();
    fbState[0] = fbState[1] = 0.0f;
    depthS.reset (sampleRate, 0.03, depth.get() / 100.0f);
    delayS.reset (sampleRate, 0.05, delayMs.get());
    fbS.reset (sampleRate, 0.03, feedback.get() / 100.0f);
    mixS.reset (sampleRate, 0.02, mix.get() / 100.0f);
}

void FlangerEffect::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext&) noexcept
{
    depthS.setTarget (depth.get() / 100.0f);
    delayS.setTarget (delayMs.get());
    fbS.setTarget (feedback.get() / 100.0f);
    mixS.setTarget (mix.get() / 100.0f);
    const double inc = rate.get() / sampleRate;
    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        const double base = delayS.next() * 0.001 * sampleRate;
        const double d = depthS.next();
        const float fb = fbS.next();
        const double p = lfo.phase;
        lfo.next (inc, dsp::Lfo::Shape::Triangle);
        float* chans[2] = { L, R };
        for (int ch = 0; ch < 2; ++ch)
        {
            const double v = 0.5 + 0.5 * std::sin (2.0 * dsp::pi * (p + 0.25 * ch));
            const double delay = juce::jmax (2.0, base * (1.0 - 0.9 * d) + base * 1.8 * d * v);
            const float wet = line.readCubic (ch, delay);
            line.push (ch, chans[ch][i] + std::tanh (fb * wet));
            const float m = mixS.current();
            chans[ch][i] += m * (wet - chans[ch][i]);
        }
        mixS.next();
        line.advance();
    }
}

// ===========================================================================
// Phaser
// ===========================================================================
PhaserEffect::PhaserEffect()
    : EffectProcessor ("phaser", "Phaser"),
      rate (addParameter (ParameterSpec::continuous ("rate", "Rate", 0.02f, 5.0f, 0.4f, Unit::Hertz, 0.5f, "Sweep speed"))),
      depth (addParameter (ParameterSpec::continuous ("depth", "Depth", 0.0f, 100.0f, 70.0f, Unit::Percent, 0.0f, "Sweep range (up to +/-3 octaves)"))),
      centre (addParameter (ParameterSpec::continuous ("centre", "Centre", 100.0f, 5000.0f, 800.0f, Unit::Hertz, 800.0f, "Centre of the sweep"))),
      feedback (addParameter (ParameterSpec::continuous ("feedback", "Feedback", 0.0f, 95.0f, 40.0f, Unit::Percent, 0.0f, "Resonance"))),
      stages (addParameter (ParameterSpec::choice ("stages", "Stages", { "4", "6", "8", "12" }, 1, "Number of notches x2"))),
      mix (addParameter (ParameterSpec::continuous ("mix", "Mix", 0.0f, 100.0f, 50.0f, Unit::Percent, 0.0f, "Dry/wet")))
{
}

void PhaserEffect::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    reset();
}

void PhaserEffect::reset() noexcept
{
    lfo.reset();
    for (auto& ch : z) ch.fill (0.0f);
    fbState[0] = fbState[1] = 0.0f;
    mixS.reset (sampleRate, 0.02, mix.get() / 100.0f);
    fbS.reset (sampleRate, 0.03, feedback.get() / 100.0f);
}

void PhaserEffect::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext&) noexcept
{
    static const int stageCounts[] = { 4, 6, 8, 12 };
    const int numStages = stageCounts[juce::jlimit (0, 3, stages.getIndex())];
    mixS.setTarget (mix.get() / 100.0f);
    fbS.setTarget (feedback.get() / 100.0f);
    const double inc = rate.get() / sampleRate;
    const double octaves = 3.0 * depth.get() / 100.0;
    const double fc = centre.get();
    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    float* chans[2] = { L, R };
    for (int i = 0; i < n; ++i)
    {
        if ((i & 7) == 0)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                const double v = std::sin (2.0 * dsp::pi * (lfo.phase + 0.25 * ch));
                const double f = juce::jlimit (20.0, sampleRate * 0.45, fc * std::pow (2.0, octaves * v));
                const double t = std::tan (dsp::pi * f / sampleRate);
                coeff[ch] = (float) ((t - 1.0) / (t + 1.0));
            }
        }
        lfo.next (inc, dsp::Lfo::Shape::Sine);
        const float fb = fbS.next();
        const float m = mixS.next();
        for (int ch = 0; ch < 2; ++ch)
        {
            const float x = chans[ch][i];
            float y = x + fb * fbState[ch];
            for (int s = 0; s < numStages; ++s)
            {
                auto& st = z[(size_t) ch][(size_t) s];
                const float out = coeff[ch] * y + st;
                st = y - coeff[ch] * out;
                y = out;
            }
            fbState[ch] = std::tanh (y);
            chans[ch][i] = x + m * (0.5f * (x + y) - x);   // phaser = dry + all-passed, notches at 100% mix
        }
    }
}

// ===========================================================================
// LFO effects
// ===========================================================================
LfoEffectBase::LfoEffectBase (juce::String id, juce::String name, float defaultRate)
    : EffectProcessor (std::move (id), std::move (name)),
      rate (addParameter (ParameterSpec::continuous ("rate", "Rate", 0.1f, 20.0f, defaultRate, Unit::Hertz, 3.0f, "Speed when not synced"))),
      sync (addParameter (ParameterSpec::onOff ("sync", "Sync", false, "Lock the speed to the project tempo"))),
      division (addParameter (ParameterSpec::choice ("division", "Division", dsp::noteDivisionNames(), 7, "Note value per cycle when synced"))),
      depth (addParameter (ParameterSpec::continuous ("depth", "Depth", 0.0f, 100.0f, 60.0f, Unit::Percent, 0.0f, "Amount"))),
      shape (addParameter (ParameterSpec::choice ("shape", "Shape", { "Sine", "Triangle", "Square" }, 0, "LFO waveform")))
{
}

bool LfoEffectBase::isParameterRelevant (const juce::String& id) const
{
    if (id == "rate")     return ! sync.getBool();
    if (id == "division") return sync.getBool();
    return true;
}

double LfoEffectBase::cyclesPerSample (const ProcessContext& ctx) const noexcept
{
    if (sync.getBool())
        return 1.0 / (dsp::divisionSeconds (division.getIndex(), ctx.bpm) * sampleRate);
    return rate.get() / sampleRate;
}

TremoloEffect::TremoloEffect() : LfoEffectBase ("tremolo", "Tremolo", 5.0f) {}

void TremoloEffect::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    reset();
}

void TremoloEffect::reset() noexcept
{
    lfo.reset();
    depthS.reset (sampleRate, 0.02, depth.get() / 100.0f);
}

void TremoloEffect::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext& ctx) noexcept
{
    depthS.setTarget (depth.get() / 100.0f);
    const double inc = cyclesPerSample (ctx);
    const auto sh = (dsp::Lfo::Shape) juce::jlimit (0, 2, shape.getIndex());
    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        const float v = lfo.next (inc, sh);
        const float g = 1.0f - depthS.next() * (0.5f - 0.5f * v);
        L[i] *= g;
        R[i] *= g;
    }
}

AutoPanEffect::AutoPanEffect() : LfoEffectBase ("autopan", "Auto-Pan", 1.0f) {}

void AutoPanEffect::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    reset();
}

void AutoPanEffect::reset() noexcept
{
    lfo.reset();
    depthS.reset (sampleRate, 0.02, depth.get() / 100.0f);
}

void AutoPanEffect::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext& ctx) noexcept
{
    depthS.setTarget (depth.get() / 100.0f);
    const double inc = cyclesPerSample (ctx);
    const auto sh = (dsp::Lfo::Shape) juce::jlimit (0, 2, shape.getIndex());
    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        const float v = lfo.next (inc, sh) * depthS.next();
        // Equal-power pan law; v = 0 is centre (both sides at -3 dB, compensated to unity sum).
        const float angle = juce::MathConstants<float>::pi * 0.25f * (1.0f + v);
        const float gl = std::cos (angle) * juce::MathConstants<float>::sqrt2;
        const float gr = std::sin (angle) * juce::MathConstants<float>::sqrt2;
        const float mono = 0.5f * (L[i] + R[i]);
        const float side = 0.5f * (L[i] - R[i]);
        L[i] = (mono + side) * gl;
        R[i] = (mono - side) * gr;
    }
}
} // namespace context::fx
