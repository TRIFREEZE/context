#pragma once

#include "DSP/EffectProcessor.h"
#include "DspHelpers.h"
#include <vector>

namespace context::fx
{
/** Feed-forward stereo-linked compressor with soft knee, peak or RMS detection. */
class CompressorEffect final : public EffectProcessor
{
public:
    CompressorEffect();
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;
    std::optional<float> getGainReductionDb() const noexcept override { return meter.load(); }

    /** Static curve: output level for an input level (dB), for tests and the UI. */
    static float gainComputerDb (float inputDb, float threshold, float ratio, float knee) noexcept;

private:
    EffectParameter &threshold, &ratio, &attack, &release, &knee, &makeup, &mix, &detector;
    float grDb = 0.0f, rmsState = 0.0f;
    SmoothedParam makeupS, mixS;
    std::atomic<float> meter { 0.0f };
};

/** Look-ahead brick-wall limiter (1.5 ms look-ahead, reported as latency). */
class LimiterEffect final : public EffectProcessor
{
public:
    LimiterEffect();
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override { return lookahead; }
    std::optional<float> getGainReductionDb() const noexcept override { return meter.load(); }

private:
    EffectParameter &drive, &ceiling, &release;
    int lookahead = 64;
    dsp::DelayLine delay;
    std::vector<float> required;       // required gain for the last lookahead+1 input samples
    int ringPos = 0;
    float gain = 1.0f;
    SmoothedParam driveS, ceilingS;
    std::atomic<float> meter { 0.0f };
};

/** Noise gate with hold, hysteresis and adjustable range. */
class GateEffect final : public EffectProcessor
{
public:
    GateEffect();
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;
    std::optional<float> getGainReductionDb() const noexcept override { return meter.load(); }

private:
    EffectParameter &threshold, &range, &attack, &hold, &release;
    float env = 0.0f, gainState = 0.0f;
    int holdCounter = 0;
    bool open = false;
    std::atomic<float> meter { 0.0f };
};

/** Envelope-difference transient designer: more/less attack and sustain. */
class TransientEffect final : public EffectProcessor
{
public:
    TransientEffect();
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;

private:
    EffectParameter &attackAmt, &sustainAmt, &output;
    dsp::EnvelopeFollower fastA, slowA, longS, shortS;
    float gainSmooth = 1.0f, gainCoeff = 0.0f;
    SmoothedParam outS;
};
} // namespace context::fx
