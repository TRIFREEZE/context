#include "SpaceEffects.h"

namespace context::fx
{
namespace
{
    // Mutually prime-ish base lengths (ms) avoid stacked resonances.
    constexpr double baseLineMs[8] = { 31.7, 37.9, 41.3, 47.1, 53.9, 59.3, 67.1, 73.7 };
    constexpr double baseDiffMs[4] = { 4.7, 6.9, 9.3, 12.1 };
    constexpr double modDepthMs = 0.35;
    constexpr double maxSizeScale = 1.6;
}

// ===========================================================================
// Reverb
// ===========================================================================
ReverbEffect::ReverbEffect()
    : EffectProcessor ("reverb", "Reverb"),
      size (addParameter (ParameterSpec::continuous ("size", "Size", 0.0f, 100.0f, 55.0f, Unit::Percent, 0.0f, "Room size (spacing of early reflections)"))),
      decay (addParameter (ParameterSpec::continuous ("decay", "Decay", 0.2f, 20.0f, 1.8f, Unit::Seconds, 2.0f, "Reverb time (RT60)"))),
      predelay (addParameter (ParameterSpec::continuous ("predelay", "Pre-delay", 0.0f, 250.0f, 15.0f, Unit::Milliseconds, 50.0f, "Gap before the reverb starts"))),
      damping (addParameter (ParameterSpec::continuous ("damping", "Damping", 0.0f, 100.0f, 45.0f, Unit::Percent, 0.0f, "How quickly highs die away"))),
      width (addParameter (ParameterSpec::continuous ("width", "Width", 0.0f, 100.0f, 100.0f, Unit::Percent, 0.0f, "Stereo width of the reverb"))),
      mix (addParameter (ParameterSpec::continuous ("mix", "Mix", 0.0f, 100.0f, 25.0f, Unit::Percent, 0.0f, "Dry/wet")))
{
}

void ReverbEffect::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    for (int i = 0; i < numLines; ++i)
    {
        lines[(size_t) i].prepare (1, (int) ((baseLineMs[i] * maxSizeScale + modDepthMs * 2.0 + 2.0) * 0.001 * sr));
        mod[(size_t) i].reset (i / (double) numLines);
    }
    for (int i = 0; i < numDiffusers; ++i)
        diffusers[(size_t) i].prepare (2, (int) ((baseDiffMs[i] * maxSizeScale + 1.0) * 0.001 * sr));
    preDelayLine.prepare (2, (int) (0.26 * sr));
    inputHp.set (dsp::Biquad::Type::HighPass, 90.0, 0.707, 0.0, sr);
    reset();
}

void ReverbEffect::updateGeometry() noexcept
{
    const float s = size.get() / 100.0f;
    const float rt = juce::jmax (0.1f, decay.get());
    if (s == lastSize && rt == lastDecay)
        return;
    lastSize = s;
    lastDecay = rt;
    const double scale = 0.45 + (maxSizeScale - 0.45) * s;
    for (int i = 0; i < numLines; ++i)
    {
        lineLength[(size_t) i] = baseLineMs[i] * scale * 0.001 * sampleRate;
        // Per-line gain so that every line decays 60 dB in rt seconds.
        lineGain[(size_t) i] = (float) std::pow (10.0, -3.0 * lineLength[(size_t) i] / (rt * sampleRate));
    }
    for (int i = 0; i < numDiffusers; ++i)
        diffLength[(size_t) i] = baseDiffMs[i] * (0.6 + 0.4 * s / 1.0) * 0.001 * sampleRate;
}

void ReverbEffect::reset() noexcept
{
    for (auto& l : lines) l.reset();
    for (auto& d : diffusers) d.reset();
    preDelayLine.reset();
    inputHp.reset();
    dampState.fill (0.0f);
    lastSize = lastDecay = -1.0f;
    updateGeometry();
    mixS.reset (sampleRate, 0.02, mix.get() / 100.0f);
    widthS.reset (sampleRate, 0.02, width.get() / 100.0f);
    preS.reset (sampleRate, 0.05, (float) (predelay.get() * 0.001 * sampleRate));
}

void ReverbEffect::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext&) noexcept
{
    updateGeometry();
    mixS.setTarget (mix.get() / 100.0f);
    widthS.setTarget (width.get() / 100.0f);
    preS.setTarget ((float) (predelay.get() * 0.001 * sampleRate));
    // Damping 0..100% -> one-pole low-pass coefficient inside the feedback loop.
    dampCoeff = 0.05f + 0.8f * damping.get() / 100.0f;
    const double modInc = 0.0, modDepth = modDepthMs * 0.001 * sampleRate;
    juce::ignoreUnused (modInc);

    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        const float dryL = L[i], dryR = R[i];

        // Pre-delay, then 4 series allpass diffusers per channel.
        preDelayLine.push (0, inputHp.process (dryL, 0));
        preDelayLine.push (1, inputHp.process (dryR, 1));
        const double pd = preS.next();
        float inL = preDelayLine.readLinear (0, pd), inR = preDelayLine.readLinear (1, pd);
        preDelayLine.advance();
        for (int d = 0; d < numDiffusers; ++d)
        {
            auto& ap = diffusers[(size_t) d];
            constexpr float g = 0.62f;
            const float dl = ap.readLinear (0, diffLength[(size_t) d]), dr = ap.readLinear (1, diffLength[(size_t) d]);
            const float yl = -g * inL + dl, yr = -g * inR + dr;
            ap.push (0, inL + g * yl);
            ap.push (1, inR + g * yr);
            ap.advance();
            inL = yl;
            inR = yr;
        }

        // Read the delay lines (slowly modulated), damp and decay.
        std::array<float, numLines> y {};
        for (int k = 0; k < numLines; ++k)
        {
            const double m = mod[(size_t) k].next ((0.11 + 0.07 * k) / sampleRate, dsp::Lfo::Shape::Sine) * modDepth;
            float v = lines[(size_t) k].readLinear (0, lineLength[(size_t) k] + modDepth + m);
            dampState[(size_t) k] = v + dampCoeff * (dampState[(size_t) k] - v);
            y[(size_t) k] = dampState[(size_t) k] * lineGain[(size_t) k];
        }

        // Householder feedback matrix: x - (2/N) * sum(x). Energy preserving, dense mixing.
        float sum = 0.0f;
        for (auto v : y) sum += v;
        const float h = sum * (2.0f / numLines);
        for (int k = 0; k < numLines; ++k)
        {
            const float in = (k & 1) ? inR : inL;
            lines[(size_t) k].push (0, y[(size_t) k] - h + in * 0.5f);
            lines[(size_t) k].advance();
        }

        float wl = (y[0] - y[2] + y[4] - y[6]) * 0.6f;
        float wr = (y[1] - y[3] + y[5] - y[7]) * 0.6f;
        const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * widthS.next();
        wl = mid + side;
        wr = mid - side;

        const float m = mixS.next();
        L[i] = dryL + m * (wl - dryL);
        R[i] = dryR + m * (wr - dryR);
    }
}

// ===========================================================================
// Delay
// ===========================================================================
DelayEffect::DelayEffect()
    : EffectProcessor ("delay", "Delay"),
      sync (addParameter (ParameterSpec::onOff ("sync", "Sync", true, "Lock the delay time to the project tempo"))),
      division (addParameter (ParameterSpec::choice ("division", "Division", dsp::noteDivisionNames(), 8, "Note value when synced"))),
      time (addParameter (ParameterSpec::continuous ("time", "Time", 1.0f, 2000.0f, 300.0f, Unit::Milliseconds, 250.0f, "Delay time when not synced"))),
      feedback (addParameter (ParameterSpec::continuous ("feedback", "Feedback", 0.0f, 95.0f, 35.0f, Unit::Percent, 0.0f, "How many repeats"))),
      lowCut (addParameter (ParameterSpec::continuous ("lowcut", "Low Cut", 20.0f, 2000.0f, 150.0f, Unit::Hertz, 200.0f, "Thin out the repeats"))),
      highCut (addParameter (ParameterSpec::continuous ("highcut", "High Cut", 1000.0f, 20000.0f, 8000.0f, Unit::Hertz, 5000.0f, "Darken the repeats"))),
      mode (addParameter (ParameterSpec::choice ("mode", "Mode", { "Stereo", "Ping-Pong", "Wide" }, 0,
                                                 "Stereo: each side repeats on itself. Ping-Pong: repeats bounce L/R. Wide: right side is offset."))),
      mix (addParameter (ParameterSpec::continuous ("mix", "Mix", 0.0f, 100.0f, 25.0f, Unit::Percent, 0.0f, "Dry/wet")))
{
}

bool DelayEffect::isParameterRelevant (const juce::String& id) const
{
    if (id == "division") return sync.getBool();
    if (id == "time")     return ! sync.getBool();
    return true;
}

double DelayEffect::getTailSeconds() const noexcept
{
    const double t = sync.getBool() ? dsp::divisionSeconds (division.getIndex(), lastBpm) : time.get() * 0.001;
    const double fb = juce::jlimit (0.0, 0.95, feedback.get() / 100.0);
    const double repeats = fb > 0.01 ? std::log (0.001) / std::log (fb) : 1.0;   // until -60 dB
    return juce::jmin (30.0, t * (repeats + 1.0));
}

void DelayEffect::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    line.prepare (2, (int) (maxDelaySeconds * 1.3 * sr) + 8);
    reset();
}

void DelayEffect::reset() noexcept
{
    line.reset();
    for (auto& f : hp) f.reset();
    for (auto& f : lp) f.reset();
    lastLow = lastHigh = -1.0f;
    delayS.reset (sampleRate, 0.08);
    const double t = sync.getBool() ? dsp::divisionSeconds (division.getIndex(), lastBpm) : time.get() * 0.001;
    delayS.setCurrentAndTargetValue (juce::jlimit (1.0, maxDelaySeconds * sampleRate, t * sampleRate));
    fbS.reset (sampleRate, 0.03, feedback.get() / 100.0f);
    mixS.reset (sampleRate, 0.02, mix.get() / 100.0f);
}

void DelayEffect::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext& ctx) noexcept
{
    lastBpm = ctx.bpm;
    const double t = sync.getBool() ? dsp::divisionSeconds (division.getIndex(), ctx.bpm) : time.get() * 0.001;
    delayS.setTargetValue (juce::jlimit (1.0, maxDelaySeconds * sampleRate, t * sampleRate));
    fbS.setTarget (feedback.get() / 100.0f);
    mixS.setTarget (mix.get() / 100.0f);
    if (lowCut.get() != lastLow || highCut.get() != lastHigh)
    {
        lastLow = lowCut.get();
        lastHigh = highCut.get();
        for (int ch = 0; ch < 2; ++ch)
        {
            hp[(size_t) ch].set (dsp::Biquad::Type::HighPass, lastLow, 0.707, 0.0, sampleRate);
            lp[(size_t) ch].set (dsp::Biquad::Type::LowPass, lastHigh, 0.707, 0.0, sampleRate);
        }
    }
    const int m = mode.getIndex();
    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        const double d = delayS.getNextValue();
        const double dR = m == 2 ? juce::jmin (maxDelaySeconds * 1.25 * sampleRate, d * 1.25) : d;
        float tl = line.readCubic (0, d);
        float tr = line.readCubic (1, dR);
        // Filter the repeats (both on what you hear and on what feeds back).
        tl = lp[0].process (hp[0].process (tl, 0), 0);
        tr = lp[1].process (hp[1].process (tr, 0), 0);
        const float fb = fbS.next();
        float inL = L[i], inR = R[i];
        float writeL, writeR;
        if (m == 1)   // ping-pong: mono input enters left, repeats cross over
        {
            writeL = 0.5f * (inL + inR) + std::tanh (tr * fb);
            writeR = std::tanh (tl * fb);
        }
        else
        {
            writeL = inL + std::tanh (tl * fb);
            writeR = inR + std::tanh (tr * fb);
        }
        line.push (0, writeL);
        line.push (1, writeR);
        line.advance();
        const float w = mixS.next();
        L[i] = inL + w * (tl - inL);
        R[i] = inR + w * (tr - inR);
    }
}
} // namespace context::fx
