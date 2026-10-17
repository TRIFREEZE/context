#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <complex>

/** Small allocation-free DSP building blocks shared by the built-in effects. */
namespace context::fx::dsp
{
inline constexpr double pi = juce::MathConstants<double>::pi;

inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g) noexcept  { return 20.0f * std::log10 (juce::jmax (g, 1.0e-9f)); }

/** One-pole smoothing coefficient for a time constant in milliseconds. */
inline float timeCoeff (float ms, double sampleRate) noexcept
{
    return ms <= 0.0f ? 0.0f : (float) std::exp (-1.0 / (0.001 * ms * sampleRate));
}

/** RBJ-cookbook biquad, transposed direct form II, two channels. Coefficients are
    computed in place (no allocation), so they can be updated on the audio thread. */
struct Biquad
{
    enum class Type { Bell, LowShelf, HighShelf, LowPass, HighPass, Notch, BandPass, AllPass };

    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] {}, z2[2] {};

    void reset() noexcept { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }

    void set (Type t, double freq, double q, double gainDb, double sr) noexcept
    {
        freq = juce::jlimit (10.0, sr * 0.49, freq);
        q = juce::jmax (0.025, q);
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = 2.0 * pi * freq / sr;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * q);
        double B0 = 1, B1 = 0, B2 = 0, A0 = 1, A1 = 0, A2 = 0;
        switch (t)
        {
            case Type::Bell:
                B0 = 1 + alpha * A; B1 = -2 * cw; B2 = 1 - alpha * A;
                A0 = 1 + alpha / A; A1 = -2 * cw; A2 = 1 - alpha / A; break;
            case Type::LowShelf:
            {
                const double s = 2.0 * std::sqrt (A) * alpha;
                B0 = A * ((A + 1) - (A - 1) * cw + s); B1 = 2 * A * ((A - 1) - (A + 1) * cw); B2 = A * ((A + 1) - (A - 1) * cw - s);
                A0 = (A + 1) + (A - 1) * cw + s;       A1 = -2 * ((A - 1) + (A + 1) * cw);    A2 = (A + 1) + (A - 1) * cw - s; break;
            }
            case Type::HighShelf:
            {
                const double s = 2.0 * std::sqrt (A) * alpha;
                B0 = A * ((A + 1) + (A - 1) * cw + s); B1 = -2 * A * ((A - 1) + (A + 1) * cw); B2 = A * ((A + 1) + (A - 1) * cw - s);
                A0 = (A + 1) - (A - 1) * cw + s;       A1 = 2 * ((A - 1) - (A + 1) * cw);      A2 = (A + 1) - (A - 1) * cw - s; break;
            }
            case Type::LowPass:
                B0 = (1 - cw) / 2; B1 = 1 - cw; B2 = (1 - cw) / 2; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case Type::HighPass:
                B0 = (1 + cw) / 2; B1 = -(1 + cw); B2 = (1 + cw) / 2; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case Type::Notch:
                B0 = 1; B1 = -2 * cw; B2 = 1; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case Type::BandPass:
                B0 = alpha; B1 = 0; B2 = -alpha; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case Type::AllPass:
                B0 = 1 - alpha; B1 = -2 * cw; B2 = 1 + alpha; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
        }
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }

    inline float process (float x, int ch) noexcept
    {
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return (float) y;
    }

    /** Magnitude response at `freq` (for drawing EQ curves on the message thread). */
    double magnitude (double freq, double sr) const noexcept
    {
        const double w = 2.0 * pi * freq / sr;
        const std::complex<double> z = std::polar (1.0, -w);
        const auto num = b0 + b1 * z + b2 * z * z;
        const auto den = 1.0 + a1 * z + a2 * z * z;
        return std::abs (num / den);
    }
};

/** Zero-delay-feedback state-variable filter (Simper/TPT), per channel. */
struct Svf
{
    double ic1[2] {}, ic2[2] {};
    double g = 0.1, k = 1.4, a1 = 0, a2 = 0, a3 = 0;

    void reset() noexcept { ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0.0; }
    void set (double freq, double q, double sr) noexcept
    {
        g = std::tan (pi * juce::jlimit (10.0, sr * 0.49, freq) / sr);
        k = 1.0 / juce::jmax (0.05, q);
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    struct Out { double low, band, high; };
    inline Out process (double x, int ch) noexcept
    {
        const double v3 = x - ic2[ch];
        const double v1 = a1 * ic1[ch] + a2 * v3;
        const double v2 = ic2[ch] + a2 * ic1[ch] + a3 * v3;
        ic1[ch] = 2.0 * v1 - ic1[ch];
        ic2[ch] = 2.0 * v2 - ic2[ch];
        return { v2, v1, x - k * v1 - v2 };
    }
};

/** Preallocated circular delay with linear or cubic fractional reads. */
class DelayLine
{
public:
    void prepare (int channels, int maxDelaySamples)
    {
        size = juce::nextPowerOfTwo (juce::jmax (4, maxDelaySamples + 4));
        mask = size - 1;
        buf.setSize (channels, size);
        reset();
    }
    void reset() noexcept { buf.clear(); write = 0; }
    int capacity() const noexcept { return size - 4; }

    inline void push (int ch, float x) noexcept { buf.setSample (ch, write, x); }
    inline void advance() noexcept              { write = (write + 1) & mask; }

    /** Delay in samples (>= 0) relative to the sample most recently pushed. */
    inline float readLinear (int ch, double delay) const noexcept
    {
        delay = juce::jlimit (0.0, (double) capacity(), delay);
        const double rp = (double) write - delay;
        const int i0 = (int) std::floor (rp);
        const float f = (float) (rp - i0);
        const float* d = buf.getReadPointer (ch);
        const float a = d[i0 & mask], b = d[(i0 + 1) & mask];
        return a + f * (b - a);
    }
    inline float readCubic (int ch, double delay) const noexcept
    {
        delay = juce::jlimit (1.0, (double) capacity() - 2.0, delay);
        const double rp = (double) write - delay;
        const int i1 = (int) std::floor (rp);
        const float f = (float) (rp - i1);
        const float* d = buf.getReadPointer (ch);
        const float y0 = d[(i1 - 1) & mask], y1 = d[i1 & mask], y2 = d[(i1 + 1) & mask], y3 = d[(i1 + 2) & mask];
        const float c0 = y1, c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + c0;
    }
    inline float readInt (int ch, int delay) const noexcept
    {
        return buf.getSample (ch, (write - juce::jlimit (0, capacity(), delay)) & mask);
    }

private:
    juce::AudioBuffer<float> buf;
    int size = 4, mask = 3, write = 0;
};

/** Peak envelope follower with separate attack/release. */
struct EnvelopeFollower
{
    float env = 0.0f, att = 0.0f, rel = 0.0f;
    void setTimes (float attackMs, float releaseMs, double sr) noexcept { att = timeCoeff (attackMs, sr); rel = timeCoeff (releaseMs, sr); }
    void reset() noexcept { env = 0.0f; }
    inline float process (float x) noexcept
    {
        const float c = x > env ? att : rel;
        env = x + c * (env - x);
        return env;
    }
};

/** Phase-accumulating LFO. */
struct Lfo
{
    enum class Shape { Sine, Triangle, Square };
    double phase = 0.0;
    void reset (double p = 0.0) noexcept { phase = p; }
    /** Returns -1..1 and advances by `inc` cycles. */
    inline float next (double inc, Shape shape, double offset = 0.0) noexcept
    {
        double p = phase + offset;
        p -= std::floor (p);
        phase += inc;
        if (phase >= 1.0) phase -= std::floor (phase);
        switch (shape)
        {
            case Shape::Sine:     return (float) std::sin (2.0 * pi * p);
            case Shape::Triangle: return (float) (p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p);
            case Shape::Square:   return (float) std::tanh (8.0 * std::sin (2.0 * pi * p));   // softened edges (no clicks)
        }
        return 0.0f;
    }
};

/** Tempo-sync note divisions (lengths in quarter notes). */
inline const juce::StringArray& noteDivisionNames()
{
    static const juce::StringArray names { "1/1", "1/2", "1/2 D", "1/2 T", "1/4", "1/4 D", "1/4 T", "1/8", "1/8 D", "1/8 T", "1/16", "1/16 D", "1/16 T", "1/32" };
    return names;
}
inline double noteDivisionQuarters (int index) noexcept
{
    static const double q[] = { 4.0, 2.0, 3.0, 4.0 / 3.0, 1.0, 1.5, 2.0 / 3.0, 0.5, 0.75, 1.0 / 3.0, 0.25, 0.375, 1.0 / 6.0, 0.125 };
    return q[juce::jlimit (0, 13, index)];
}
inline double divisionSeconds (int index, double bpm) noexcept
{
    return noteDivisionQuarters (index) * 60.0 / juce::jlimit (20.0, 999.0, bpm);
}

/** Simple one-pole DC blocker. */
struct DcBlocker
{
    float x1[2] {}, y1[2] {}, r = 0.995f;
    void prepare (double sr) noexcept { r = (float) std::exp (-2.0 * pi * 10.0 / sr); reset(); }
    void reset() noexcept { x1[0] = x1[1] = y1[0] = y1[1] = 0.0f; }
    inline float process (float x, int ch) noexcept
    {
        const float y = x - x1[ch] + r * y1[ch];
        x1[ch] = x;
        y1[ch] = y;
        return y;
    }
};
} // namespace context::fx::dsp
