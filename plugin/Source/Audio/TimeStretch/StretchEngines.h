#pragma once

#include "TimeStretchEngine.h"

namespace context::stretch
{
/** Shared pitch handling: stretch by ratio * pitchFactor, then resample to the final
    length, which shifts pitch by pitchFactor while keeping the requested duration. */
class PitchedEngine : public TimeStretchEngine
{
public:
    void prepare (double sr, int channels) override { sampleRate = sr; numChannels = channels; }
    void setRatio (double r) override                { ratio = juce::jlimit (0.05, 20.0, r); }
    void setPitchSemitones (double s) override       { semitones = juce::jlimit (-24.0, 24.0, s); }
    juce::AudioBuffer<float> process (const juce::AudioBuffer<float>&, const Progress&) override;

protected:
    /** Pure time stretch by `timeRatio`; result length llround(len * timeRatio). */
    virtual juce::AudioBuffer<float> stretch (const juce::AudioBuffer<float>&, double timeRatio, const Progress&) = 0;

    double sampleRate = 44100.0, ratio = 1.0, semitones = 0.0;
    int numChannels = 2;
};

/** Phase vocoder with identity phase locking (Laroche & Dolson), stereo phase coherence
    (phases are propagated on the mid signal and per-channel phase offsets are kept) and
    optional transient phase reset. */
class PhaseVocoderEngine final : public PitchedEngine
{
public:
    struct Config { int fftSize = 4096; int overlap = 4; bool transientReset = true; float transientThreshold = 0.35f; juce::String label; };
    explicit PhaseVocoderEngine (Config);
    juce::String name() const override { return cfg.label; }

protected:
    juce::AudioBuffer<float> stretch (const juce::AudioBuffer<float>&, double timeRatio, const Progress&) override;

private:
    Config cfg;
};

/** Waveform-similarity overlap-add. Time domain: no phasiness, keeps pitch periods. */
class WsolaEngine final : public PitchedEngine
{
public:
    struct Config { double frameMs = 30.0; double toleranceMs = 12.0; juce::String label; };
    explicit WsolaEngine (Config);
    juce::String name() const override { return cfg.label; }

    /** Exposed for the transient engine (stretches a segment). */
    static juce::AudioBuffer<float> wsola (const juce::AudioBuffer<float>& in, juce::int64 outLength, double sampleRate, const Config&, const Progress* = nullptr);

protected:
    juce::AudioBuffer<float> stretch (const juce::AudioBuffer<float>&, double timeRatio, const Progress&) override;

private:
    Config cfg;
};

/** Onset slicing: each attack is copied verbatim to its new (scaled) position and only
    the material after the attack is time-scaled to fill the slot. */
class TransientSliceEngine final : public PitchedEngine
{
public:
    juce::String name() const override { return "Percussive"; }

protected:
    juce::AudioBuffer<float> stretch (const juce::AudioBuffer<float>&, double timeRatio, const Progress&) override;
};

/** Tape-style resampling (tempo and pitch together). Pitch control is ignored. */
class RepitchEngine final : public TimeStretchEngine
{
public:
    juce::String name() const override { return "Repitch"; }
    void prepare (double, int) override {}
    void setRatio (double r) override { ratio = juce::jlimit (0.05, 20.0, r); }
    void setPitchSemitones (double) override {}
    juce::AudioBuffer<float> process (const juce::AudioBuffer<float>&, const Progress&) override;

private:
    double ratio = 1.0;
};
} // namespace context::stretch
