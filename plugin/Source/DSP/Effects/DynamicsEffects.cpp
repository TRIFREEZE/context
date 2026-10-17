#include "DynamicsEffects.h"

namespace context::fx
{
// ===========================================================================
// Compressor
// ===========================================================================
CompressorEffect::CompressorEffect()
    : EffectProcessor ("compressor", "Compressor"),
      threshold (addParameter (ParameterSpec::continuous ("threshold", "Threshold", -60.0f, 0.0f, -18.0f, Unit::Decibels, 0.0f, "Level above which compression starts"))),
      ratio (addParameter (ParameterSpec::continuous ("ratio", "Ratio", 1.0f, 20.0f, 4.0f, Unit::Ratio, 4.0f, "Amount of compression"))),
      attack (addParameter (ParameterSpec::continuous ("attack", "Attack", 0.1f, 200.0f, 10.0f, Unit::Milliseconds, 10.0f, "How fast it reacts"))),
      release (addParameter (ParameterSpec::continuous ("release", "Release", 5.0f, 1500.0f, 120.0f, Unit::Milliseconds, 150.0f, "How fast it lets go"))),
      knee (addParameter (ParameterSpec::continuous ("knee", "Knee", 0.0f, 24.0f, 6.0f, Unit::Decibels, 0.0f, "Soft knee width"))),
      makeup (addParameter (ParameterSpec::continuous ("makeup", "Makeup", -12.0f, 24.0f, 0.0f, Unit::Decibels, 0.0f, "Gain after compression"))),
      mix (addParameter (ParameterSpec::continuous ("mix", "Mix", 0.0f, 100.0f, 100.0f, Unit::Percent, 0.0f, "Dry/wet (parallel compression)"))),
      detector (addParameter (ParameterSpec::choice ("detector", "Detector", { "Peak", "RMS" }, 0, "Peak reacts to transients, RMS to average level")))
{
}

float CompressorEffect::gainComputerDb (float x, float t, float r, float w) noexcept
{
    // Giannoulis/Massberg/Reiss soft-knee gain computer.
    if (2.0f * (x - t) < -w)
        return x;
    if (w > 0.0f && 2.0f * std::abs (x - t) <= w)
    {
        const float d = x - t + w * 0.5f;
        return x + (1.0f / r - 1.0f) * d * d / (2.0f * w);
    }
    return t + (x - t) / r;
}

void CompressorEffect::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    reset();
}

void CompressorEffect::reset() noexcept
{
    grDb = 0.0f;
    rmsState = 0.0f;
    makeupS.reset (sampleRate, 0.02, dsp::dbToGain (makeup.get()));
    mixS.reset (sampleRate, 0.02, mix.get() / 100.0f);
    meter.store (0.0f);
}

void CompressorEffect::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext&) noexcept
{
    const float t = threshold.get(), r = ratio.get(), w = knee.get();
    const float aC = dsp::timeCoeff (attack.get(), sampleRate), rC = dsp::timeCoeff (release.get(), sampleRate);
    const bool rms = detector.getIndex() == 1;
    const float rmsC = dsp::timeCoeff (10.0f, sampleRate);
    makeupS.setTarget (dsp::dbToGain (makeup.get()));
    mixS.setTarget (mix.get() / 100.0f);

    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    float maxGr = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        float level = juce::jmax (std::abs (L[i]), std::abs (R[i]));
        if (rms)
        {
            rmsState = level * level + rmsC * (rmsState - level * level);
            level = std::sqrt (rmsState);
        }
        const float inDb = dsp::gainToDb (level);
        const float targetGr = gainComputerDb (inDb, t, r, w) - inDb;   // <= 0
        // Smooth the gain reduction: attack when reducing more, release when recovering.
        const float c = targetGr < grDb ? aC : rC;
        grDb = targetGr + c * (grDb - targetGr);
        maxGr = juce::jmin (maxGr, grDb);
        const float g = dsp::dbToGain (grDb) * makeupS.next();
        const float m = mixS.next();
        L[i] = L[i] * (1.0f - m + m * g);
        R[i] = R[i] * (1.0f - m + m * g);
    }
    meter.store (maxGr);
}

// ===========================================================================
// Limiter
// ===========================================================================
LimiterEffect::LimiterEffect()
    : EffectProcessor ("limiter", "Limiter"),
      drive (addParameter (ParameterSpec::continuous ("drive", "Gain", 0.0f, 24.0f, 0.0f, Unit::Decibels, 0.0f, "Push the level into the limiter"))),
      ceiling (addParameter (ParameterSpec::continuous ("ceiling", "Ceiling", -24.0f, 0.0f, -0.3f, Unit::Decibels, 0.0f, "Maximum output peak"))),
      release (addParameter (ParameterSpec::continuous ("release", "Release", 1.0f, 1000.0f, 60.0f, Unit::Milliseconds, 60.0f, "Recovery time")))
{
}

void LimiterEffect::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    lookahead = juce::jmax (1, (int) std::round (0.0015 * sr));
    delay.prepare (2, lookahead + 2);
    required.assign ((size_t) lookahead + 1, 1.0f);
    reset();
}

void LimiterEffect::reset() noexcept
{
    delay.reset();
    std::fill (required.begin(), required.end(), 1.0f);
    ringPos = 0;
    gain = 1.0f;
    driveS.reset (sampleRate, 0.02, dsp::dbToGain (drive.get()));
    ceilingS.reset (sampleRate, 0.02, dsp::dbToGain (ceiling.get()));
    meter.store (0.0f);
}

void LimiterEffect::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext&) noexcept
{
    driveS.setTarget (dsp::dbToGain (drive.get()));
    ceilingS.setTarget (dsp::dbToGain (ceiling.get()));
    const float relC = dsp::timeCoeff (release.get(), sampleRate);
    // Attack fast enough to reach the target within the look-ahead window.
    const float attC = dsp::timeCoeff ((float) (1000.0 * lookahead / sampleRate) / 5.0f, sampleRate);
    const int window = lookahead + 1;

    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    float minGain = 1.0f;
    for (int i = 0; i < n; ++i)
    {
        const float d = driveS.next(), c = ceilingS.next();
        const float l = L[i] * d, r = R[i] * d;
        const float peak = juce::jmax (std::abs (l), std::abs (r));
        const float need = peak > c ? c / peak : 1.0f;

        // Minimum required gain over the look-ahead window (the window is ~70 samples, so a
        // direct scan is cheap and obviously correct).
        required[(size_t) ringPos] = need;
        float target = 1.0f;
        for (int k = 0; k < window; ++k)
            target = juce::jmin (target, required[(size_t) k]);

        gain = target < gain ? target + attC * (gain - target) : target + relC * (gain - target);
        gain = juce::jmin (gain, 1.0f);
        minGain = juce::jmin (minGain, gain);

        delay.push (0, l);
        delay.push (1, r);
        const float dl = delay.readInt (0, lookahead), dr = delay.readInt (1, lookahead);
        delay.advance();
        // Final safety: never exceed the ceiling, even on the fastest transients.
        L[i] = juce::jlimit (-c, c, dl * gain);
        R[i] = juce::jlimit (-c, c, dr * gain);

        ringPos = (ringPos + 1) % window;
    }
    meter.store (dsp::gainToDb (minGain));
}

// ===========================================================================
// Gate
// ===========================================================================
GateEffect::GateEffect()
    : EffectProcessor ("gate", "Gate"),
      threshold (addParameter (ParameterSpec::continuous ("threshold", "Threshold", -80.0f, 0.0f, -45.0f, Unit::Decibels, 0.0f, "Level that opens the gate"))),
      range (addParameter (ParameterSpec::continuous ("range", "Range", 0.0f, 80.0f, 60.0f, Unit::Decibels, 0.0f, "How much the closed gate attenuates"))),
      attack (addParameter (ParameterSpec::continuous ("attack", "Attack", 0.05f, 50.0f, 0.5f, Unit::Milliseconds, 2.0f, "Opening time"))),
      hold (addParameter (ParameterSpec::continuous ("hold", "Hold", 0.0f, 500.0f, 40.0f, Unit::Milliseconds, 50.0f, "Stay open this long after the signal drops"))),
      release (addParameter (ParameterSpec::continuous ("release", "Release", 5.0f, 2000.0f, 120.0f, Unit::Milliseconds, 150.0f, "Closing time")))
{
}

void GateEffect::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    reset();
}

void GateEffect::reset() noexcept
{
    env = 0.0f;
    gainState = 0.0f;
    holdCounter = 0;
    open = false;
    meter.store (0.0f);
}

void GateEffect::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext&) noexcept
{
    const float openLevel = dsp::dbToGain (threshold.get());
    const float closeLevel = dsp::dbToGain (threshold.get() - 4.0f);   // 4 dB hysteresis: no chatter
    const float floor = dsp::dbToGain (-range.get());
    const float aC = dsp::timeCoeff (attack.get(), sampleRate), rC = dsp::timeCoeff (release.get(), sampleRate);
    const float detC = dsp::timeCoeff (5.0f, sampleRate);
    const int holdSamples = (int) (hold.get() * 0.001 * sampleRate);

    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    float minG = 1.0f;
    for (int i = 0; i < n; ++i)
    {
        const float x = juce::jmax (std::abs (L[i]), std::abs (R[i]));
        env = x > env ? x : x + detC * (env - x);   // instant attack detector, 5 ms decay
        if (env >= openLevel)
        {
            open = true;
            holdCounter = holdSamples;
        }
        else if (env < closeLevel)
        {
            if (holdCounter > 0) --holdCounter;
            else open = false;
        }
        const float target = open ? 1.0f : 0.0f;
        gainState = target + (target > gainState ? aC : rC) * (gainState - target);
        const float g = floor + (1.0f - floor) * gainState;
        minG = juce::jmin (minG, g);
        L[i] *= g;
        R[i] *= g;
    }
    meter.store (dsp::gainToDb (minG));
}

// ===========================================================================
// Transient shaper
// ===========================================================================
TransientEffect::TransientEffect()
    : EffectProcessor ("transient", "Transient"),
      attackAmt (addParameter (ParameterSpec::continuous ("attack", "Attack", -100.0f, 100.0f, 0.0f, Unit::Percent, 0.0f, "Emphasise (+) or soften (-) the start of each hit"))),
      sustainAmt (addParameter (ParameterSpec::continuous ("sustain", "Sustain", -100.0f, 100.0f, 0.0f, Unit::Percent, 0.0f, "Bring up (+) or tighten (-) the body and tail"))),
      output (addParameter (ParameterSpec::continuous ("output", "Output", -18.0f, 18.0f, 0.0f, Unit::Decibels, 0.0f, "Output level")))
{
}

void TransientEffect::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    fastA.setTimes (0.3f, 60.0f, sr);    // follows the attack immediately
    slowA.setTimes (25.0f, 60.0f, sr);   // lags behind on attacks
    longS.setTimes (0.3f, 400.0f, sr);   // holds the tail
    shortS.setTimes (0.3f, 40.0f, sr);   // drops quickly after the hit
    gainCoeff = dsp::timeCoeff (1.0f, sr);
    reset();
}

void TransientEffect::reset() noexcept
{
    fastA.reset();
    slowA.reset();
    longS.reset();
    shortS.reset();
    gainSmooth = 1.0f;
    outS.reset (sampleRate, 0.02, dsp::dbToGain (output.get()));
}

void TransientEffect::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext&) noexcept
{
    const float a = attackAmt.get() / 100.0f, s = sustainAmt.get() / 100.0f;
    outS.setTarget (dsp::dbToGain (output.get()));
    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    constexpr float eps = 1.0e-6f;
    for (int i = 0; i < n; ++i)
    {
        const float x = 0.5f * (std::abs (L[i]) + std::abs (R[i]));
        const float f = fastA.process (x), sl = slowA.process (x);
        const float lg = longS.process (x), sh = shortS.process (x);
        // 0..1 "how much of this moment is attack" and "how much is sustain".
        const float transient = juce::jlimit (0.0f, 1.0f, (f - sl) / (f + eps));
        const float sustain = juce::jlimit (0.0f, 1.0f, (lg - sh) / (lg + eps));
        // Up to +/-12 dB on each component.
        const float gDb = 12.0f * (a * transient + s * sustain);
        const float target = dsp::dbToGain (juce::jlimit (-24.0f, 18.0f, gDb));
        gainSmooth = target + gainCoeff * (gainSmooth - target);
        const float g = gainSmooth * outS.next();
        L[i] *= g;
        R[i] *= g;
    }
}
} // namespace context::fx
