#include "Resampler.h"
#include <cmath>
#include <vector>

namespace context::dsp::resampler
{
namespace
{
    constexpr int zeroCrossings = 32;
    constexpr int tableOversample = 512;
    constexpr double kaiserBeta = 9.0;

    double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 64; ++k)
        {
            const double f = x / (2.0 * k);
            term *= f * f;
            sum += term;
            if (term < sum * 1.0e-12)
                break;
        }
        return sum;
    }

    /** Windowed-sinc kernel h(u), u in [0, zeroCrossings], tabulated. */
    struct Kernel
    {
        std::vector<float> table;

        Kernel()
        {
            const int n = zeroCrossings * tableOversample;
            table.resize ((size_t) n + 2);
            const double i0b = besselI0 (kaiserBeta);
            for (int i = 0; i <= n + 1; ++i)
            {
                const double u = (double) i / tableOversample;
                const double sinc = u == 0.0 ? 1.0 : std::sin (juce::MathConstants<double>::pi * u) / (juce::MathConstants<double>::pi * u);
                const double r = u / zeroCrossings;
                const double w = r >= 1.0 ? 0.0 : besselI0 (kaiserBeta * std::sqrt (1.0 - r * r)) / i0b;
                table[(size_t) i] = (float) (sinc * w);
            }
        }

        inline float operator() (double u) const noexcept
        {
            u = std::abs (u);
            if (u >= zeroCrossings)
                return 0.0f;
            const double x = u * tableOversample;
            const int i = (int) x;
            const float f = (float) (x - i);
            return table[(size_t) i] + f * (table[(size_t) i + 1] - table[(size_t) i]);
        }
    };

    const Kernel& kernel()
    {
        static const Kernel k;   // immutable after construction
        return k;
    }
}

void process (const float* in, int inLength, float* out, int outLength, double start, double step)
{
    const auto& h = kernel();
    // Downsampling: lower the cutoff to the output Nyquist (with a small guard band).
    const double cutoff = step > 1.0 ? 0.97 / step : 0.97;
    const double halfWidth = zeroCrossings / cutoff;
    for (int n = 0; n < outLength; ++n)
    {
        const double t = start + n * step;
        const int first = (int) std::ceil (t - halfWidth);
        const int last = (int) std::floor (t + halfWidth);
        double acc = 0.0;
        const int a = juce::jmax (first, 0), b = juce::jmin (last, inLength - 1);
        for (int k = a; k <= b; ++k)
            acc += (double) in[k] * h ((t - k) * cutoff);
        out[n] = (float) (acc * cutoff);
    }
}

juce::AudioBuffer<float> convertRate (const juce::AudioBuffer<float>& in, double inRate, double outRate)
{
    if (! (inRate > 0.0) || ! (outRate > 0.0) || in.getNumSamples() == 0)
        return juce::AudioBuffer<float> (juce::jmax (1, in.getNumChannels()), 0);
    if (std::abs (inRate - outRate) < 1.0e-9)
        return juce::AudioBuffer<float> (in);
    const auto outLen = (int) std::llround ((double) in.getNumSamples() * outRate / inRate);
    juce::AudioBuffer<float> out (in.getNumChannels(), juce::jmax (0, outLen));
    const double step = inRate / outRate;
    for (int ch = 0; ch < in.getNumChannels(); ++ch)
        process (in.getReadPointer (ch), in.getNumSamples(), out.getWritePointer (ch), outLen, 0.0, step);
    return out;
}

juce::AudioBuffer<float> toLength (const juce::AudioBuffer<float>& in, juce::int64 outLength)
{
    juce::AudioBuffer<float> out (juce::jmax (1, in.getNumChannels()), (int) juce::jmax<juce::int64> (0, outLength));
    out.clear();
    if (in.getNumSamples() == 0 || outLength <= 0)
        return out;
    const double step = (double) in.getNumSamples() / (double) outLength;
    for (int ch = 0; ch < in.getNumChannels(); ++ch)
        process (in.getReadPointer (ch), in.getNumSamples(), out.getWritePointer (ch), (int) outLength, 0.0, step);
    return out;
}
} // namespace context::dsp::resampler
