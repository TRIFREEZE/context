#include "SaturationEffects.h"

namespace context::fx
{
// ===========================================================================
// OversampledShaper
// ===========================================================================
void OversampledShaper::prepare (double sr, int block)
{
    sampleRate = sr;
    blockSize = block;
    oversampler = std::make_unique<juce::dsp::Oversampling<float>> (2, (size_t) oversamplingLog2(),
                                                                    juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
                                                                    true, true);
    oversampler->initProcessing ((size_t) block);
    latency = (int) std::round (oversampler->getLatencyInSamples());
    osRate = sr * (1 << oversamplingLog2());
    dryDelay.prepare (2, latency + 4);
    dry.setSize (2, block);
    reset();
}

void OversampledShaper::reset() noexcept
{
    if (oversampler != nullptr)
        oversampler->reset();
    dryDelay.reset();
    mixS.reset (sampleRate, 0.02, mixAmount());
}

void OversampledShaper::process (juce::AudioBuffer<float>& buf, int n, const ProcessContext&) noexcept
{
    if (oversampler == nullptr)
        return;
    mixS.setTarget (mixAmount());
    const bool needDry = mixS.isSmoothing() || mixS.current() < 0.9999f;

    // Latency-aligned dry copy for the mix control.
    for (int i = 0; i < n; ++i)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            dryDelay.push (ch, buf.getSample (ch, i));
            dry.setSample (ch, i, dryDelay.readInt (ch, latency));
        }
        dryDelay.advance();
    }

    beginBlock();
    juce::dsp::AudioBlock<float> block (buf.getArrayOfWritePointers(), 2, (size_t) n);
    auto up = oversampler->processSamplesUp (block);
    // Frame by frame (channel 0 then 1) so per-sample smoothing in shape() stays in step.
    auto* l = up.getChannelPointer (0);
    auto* r = up.getChannelPointer (1);
    for (size_t i = 0; i < up.getNumSamples(); ++i)
    {
        l[i] = shape (l[i], 0);
        r[i] = shape (r[i], 1);
    }
    oversampler->processSamplesDown (block);
    postProcess (buf, n);

    if (needDry)
    {
        for (int i = 0; i < n; ++i)
        {
            const float m = mixS.next();
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* w = buf.getWritePointer (ch);
                w[i] = dry.getSample (ch, i) + m * (w[i] - dry.getSample (ch, i));
            }
        }
    }
    else
    {
        mixS.skip (n);
    }
}

// ===========================================================================
// Saturation
// ===========================================================================
SaturationEffect::SaturationEffect()
    : OversampledShaper ("saturation", "Saturation"),
      drive (addParameter (ParameterSpec::continuous ("drive", "Drive", 0.0f, 36.0f, 8.0f, Unit::Decibels, 0.0f, "How hard the signal hits the saturator"))),
      character (addParameter (ParameterSpec::choice ("character", "Character", { "Tape", "Tube", "Warm Clip" }, 0,
                                                      "Tape: smooth and symmetric. Tube: adds even harmonics. Warm Clip: firmer cubic clipping."))),
      tone (addParameter (ParameterSpec::continuous ("tone", "Tone", -100.0f, 100.0f, 0.0f, Unit::Percent, 0.0f, "Tilt: darker (-) or brighter (+)"))),
      output (addParameter (ParameterSpec::continuous ("output", "Output", -24.0f, 12.0f, 0.0f, Unit::Decibels, 0.0f, "Output level"))),
      mix (addParameter (ParameterSpec::continuous ("mix", "Mix", 0.0f, 100.0f, 100.0f, Unit::Percent, 0.0f, "Dry/wet")))
{
}

void SaturationEffect::reset() noexcept
{
    OversampledShaper::reset();
    lowTilt.reset();
    highTilt.reset();
    lastTone = 1.0e9f;
    outS.reset (sampleRate, 0.02, dsp::dbToGain (output.get()));
    driveS.reset (osRate, 0.02, dsp::dbToGain (drive.get()));
}

void SaturationEffect::beginBlock() noexcept
{
    driveS.setTarget (dsp::dbToGain (drive.get()));
    mode = character.getIndex();
}

float SaturationEffect::shape (float x, int ch) noexcept
{
    if (ch == 0)
        driveGain = driveS.next();   // one step per oversampled frame
    const float g = driveGain;
    const float in = x * g;
    float y;
    switch (mode)
    {
        case 1:   // Tube: biased tanh -> even harmonics, DC removed by the bias subtraction
            y = std::tanh (in + 0.25f) - std::tanh (0.25f);
            break;
        case 2:   // Warm clip: cubic soft clipper
        {
            const float c = juce::jlimit (-1.5f, 1.5f, in);
            y = c - (4.0f / 27.0f) * c * c * c;
            break;
        }
        default:  // Tape
            y = std::tanh (in);
            break;
    }
    // Keep perceived level roughly constant as drive increases.
    return y / std::sqrt (g);
}

void SaturationEffect::postProcess (juce::AudioBuffer<float>& buf, int n) noexcept
{
    const float t = tone.get();
    if (t != lastTone)
    {
        lastTone = t;
        const double tiltDb = 6.0 * t / 100.0;
        lowTilt.set (dsp::Biquad::Type::LowShelf, 900.0, 0.6, -tiltDb, sampleRate);
        highTilt.set (dsp::Biquad::Type::HighShelf, 900.0, 0.6, tiltDb, sampleRate);
    }
    outS.setTarget (dsp::dbToGain (output.get()));
    const bool tilt = std::abs (t) > 0.01f;
    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        float l = L[i], r = R[i];
        if (tilt)
        {
            l = highTilt.process (lowTilt.process (l, 0), 0);
            r = highTilt.process (lowTilt.process (r, 1), 1);
        }
        const float o = outS.next();
        L[i] = l * o;
        R[i] = r * o;
    }
}

// ===========================================================================
// Distortion
// ===========================================================================
DistortionEffect::DistortionEffect()
    : OversampledShaper ("distortion", "Distortion"),
      drive (addParameter (ParameterSpec::continuous ("drive", "Drive", 0.0f, 48.0f, 18.0f, Unit::Decibels, 0.0f, "Amount of distortion"))),
      mode (addParameter (ParameterSpec::choice ("mode", "Mode", { "Hard Clip", "Fold", "Fuzz", "Rectify" }, 0,
                                                 "Hard Clip: square-ish. Fold: wavefolding. Fuzz: asymmetric. Rectify: octave-up grit."))),
      tone (addParameter (ParameterSpec::continuous ("tone", "Tone", 500.0f, 20000.0f, 7000.0f, Unit::Hertz, 3000.0f, "Low-pass after the distortion"))),
      output (addParameter (ParameterSpec::continuous ("output", "Output", -36.0f, 12.0f, -6.0f, Unit::Decibels, 0.0f, "Output level"))),
      mix (addParameter (ParameterSpec::continuous ("mix", "Mix", 0.0f, 100.0f, 100.0f, Unit::Percent, 0.0f, "Dry/wet")))
{
}

void DistortionEffect::reset() noexcept
{
    OversampledShaper::reset();
    toneFilter.reset();
    dc.prepare (sampleRate);
    lastTone = -1.0f;
    outS.reset (sampleRate, 0.02, dsp::dbToGain (output.get()));
    driveS.reset (osRate, 0.02, dsp::dbToGain (drive.get()));
}

void DistortionEffect::beginBlock() noexcept
{
    driveS.setTarget (dsp::dbToGain (drive.get()));
    shapeMode = mode.getIndex();
}

float DistortionEffect::shape (float x, int ch) noexcept
{
    if (ch == 0)
        driveGain = driveS.next();
    const float in = x * driveGain;
    float y;
    switch (shapeMode)
    {
        case 1:  y = std::sin (juce::MathConstants<float>::halfPi * in); break;                    // fold
        case 2:  y = in > 0.0f ? 1.0f - std::exp (-in) : -0.7f * (1.0f - std::exp (in / 0.7f)); break; // fuzz
        case 3:  y = std::tanh (std::abs (in) * 1.5f) * 1.2f - 0.6f; break;                         // rectify
        default: y = juce::jlimit (-1.0f, 1.0f, in); break;                                         // hard clip
    }
    return y;
}

void DistortionEffect::postProcess (juce::AudioBuffer<float>& buf, int n) noexcept
{
    if (tone.get() != lastTone)
    {
        lastTone = tone.get();
        toneFilter.set (dsp::Biquad::Type::LowPass, lastTone, 0.707, 0.0, sampleRate);
    }
    outS.setTarget (dsp::dbToGain (output.get()));
    const bool needDc = shapeMode == 3 || shapeMode == 2;
    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        float l = toneFilter.process (L[i], 0), r = toneFilter.process (R[i], 1);
        if (needDc) { l = dc.process (l, 0); r = dc.process (r, 1); }
        const float o = outS.next();
        L[i] = l * o;
        R[i] = r * o;
    }
}

// ===========================================================================
// Soft clipper
// ===========================================================================
SoftClipperEffect::SoftClipperEffect()
    : OversampledShaper ("clipper", "Soft Clipper"),
      inputGain (addParameter (ParameterSpec::continuous ("input", "Input", 0.0f, 24.0f, 0.0f, Unit::Decibels, 0.0f, "Push the signal into the clipper"))),
      ceiling (addParameter (ParameterSpec::continuous ("ceiling", "Ceiling", -24.0f, 0.0f, -1.0f, Unit::Decibels, 0.0f, "Highest output peak"))),
      softness (addParameter (ParameterSpec::continuous ("softness", "Softness", 0.0f, 100.0f, 40.0f, Unit::Percent, 0.0f, "0% = hard clip, 100% = very gradual knee"))),
      output (addParameter (ParameterSpec::continuous ("output", "Output", -12.0f, 12.0f, 0.0f, Unit::Decibels, 0.0f, "Output level")))
{
}

void SoftClipperEffect::reset() noexcept
{
    OversampledShaper::reset();
    outS.reset (sampleRate, 0.02, dsp::dbToGain (output.get()));
}

void SoftClipperEffect::beginBlock() noexcept
{
    gainIn = dsp::dbToGain (inputGain.get());
    ceil = dsp::dbToGain (ceiling.get());
    knee = juce::jlimit (0.0f, 0.95f, softness.get() / 100.0f * 0.9f);
}

float SoftClipperEffect::shape (float x, int) noexcept
{
    const float in = x * gainIn;
    const float a = std::abs (in);
    const float t = ceil * (1.0f - knee);   // linear region ends here
    if (a <= t)
        return in;
    const float span = ceil - t;
    const float y = span > 1.0e-6f ? t + span * std::tanh ((a - t) / span) : ceil;
    return std::copysign (juce::jmin (y, ceil), in);
}

void SoftClipperEffect::postProcess (juce::AudioBuffer<float>& buf, int n) noexcept
{
    outS.setTarget (dsp::dbToGain (output.get()));
    auto* L = buf.getWritePointer (0);
    auto* R = buf.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        const float o = outS.next();
        L[i] *= o;
        R[i] *= o;
    }
}
} // namespace context::fx
