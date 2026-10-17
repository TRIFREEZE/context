#include "SynthAudio.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

namespace context::synth
{
namespace
{
    constexpr double twoPi = juce::MathConstants<double>::twoPi;

    struct Ctx
    {
        double sr = 44100.0;
        juce::Random rng;
        int root = 9;              // pitch class
        bool minor = true;
        double bpm = 120.0;
        juce::String prompt;       // lower-case
        bool has (const char* word) const { return prompt.contains (word); }
        bool hasAny (std::initializer_list<const char*> words) const
        {
            for (auto* w : words)
                if (prompt.contains (w))
                    return true;
            return false;
        }
        float noise() { return rng.nextFloat() * 2.0f - 1.0f; }
        int scaleNote (int degree, int octave) const
        {
            static const int majorSteps[] = { 0, 2, 4, 5, 7, 9, 11 };
            static const int minorSteps[] = { 0, 2, 3, 5, 7, 8, 10 };
            const int oct = degree >= 0 ? degree / 7 : (degree - 6) / 7;
            const int idx = degree - oct * 7;
            return noteNumber (root, octave + oct) + (minor ? minorSteps[idx] : majorSteps[idx]);
        }
    };

    juce::AudioBuffer<float> makeBuffer (const Ctx& c, double seconds)
    {
        juce::AudioBuffer<float> b (2, juce::jmax (1, (int) (seconds * c.sr)));
        b.clear();
        return b;
    }

    void addMono (juce::AudioBuffer<float>& dst, const std::vector<float>& src, int offset, float gain = 1.0f)
    {
        for (int i = 0; i < (int) src.size(); ++i)
        {
            const int j = offset + i;
            if (j < 0 || j >= dst.getNumSamples())
                continue;
            dst.addSample (0, j, src[(size_t) i] * gain);
            dst.addSample (1, j, src[(size_t) i] * gain);
        }
    }

    std::vector<float> kick (Ctx& c, double len = 0.6)
    {
        std::vector<float> out ((size_t) (len * c.sr));
        double phase = 0;
        for (size_t i = 0; i < out.size(); ++i)
        {
            const double t = (double) i / c.sr;
            const double f = 45.0 + 105.0 * std::exp (-t * 30.0);
            phase += twoPi * f / c.sr;
            double s = std::sin (phase) * std::exp (-t * 6.0);
            if (t < 0.003)
                s += c.noise() * 0.5 * (1.0 - t / 0.003);
            out[i] = (float) s;
        }
        return out;
    }

    std::vector<float> snare (Ctx& c, double len = 0.35)
    {
        std::vector<float> out ((size_t) (len * c.sr));
        float hp = 0, prev = 0;
        for (size_t i = 0; i < out.size(); ++i)
        {
            const double t = (double) i / c.sr;
            const float n = c.noise();
            hp = 0.85f * (hp + n - prev);
            prev = n;
            out[i] = (float) (hp * std::exp (-t * 18.0) * 0.8 + std::sin (twoPi * 190.0 * t) * std::exp (-t * 25.0) * 0.5);
        }
        return out;
    }

    std::vector<float> hat (Ctx& c, bool open)
    {
        const double len = open ? 0.5 : 0.09;
        std::vector<float> out ((size_t) (len * c.sr));
        float p1 = 0, p2 = 0;
        for (size_t i = 0; i < out.size(); ++i)
        {
            const double t = (double) i / c.sr;
            const float n = c.noise();
            const float d1 = n - p1; p1 = n;
            const float d2 = d1 - p2; p2 = d1;
            out[i] = (float) (d2 * 0.35 * std::exp (-t * (open ? 8.0 : 45.0)));
        }
        return out;
    }

    std::vector<float> clap (Ctx& c)
    {
        std::vector<float> out ((size_t) (0.4 * c.sr));
        float hp = 0, prev = 0;
        for (size_t i = 0; i < out.size(); ++i)
        {
            const double t = (double) i / c.sr;
            double env = std::exp (-t * 14.0) * 0.6;
            for (double burst : { 0.0, 0.011, 0.022 })
                if (t >= burst && t < burst + 0.008)
                    env += std::exp (-(t - burst) * 300.0);
            const float n = c.noise();
            hp = 0.9f * (hp + n - prev);
            prev = n;
            out[i] = (float) (hp * env);
        }
        return out;
    }

    std::vector<float> cowbell (Ctx& c)
    {
        std::vector<float> out ((size_t) (0.5 * c.sr));
        for (size_t i = 0; i < out.size(); ++i)
        {
            const double t = (double) i / c.sr;
            const double a = std::sin (twoPi * 587.0 * t) > 0 ? 1.0 : -1.0;
            const double b = std::sin (twoPi * 845.0 * t) > 0 ? 1.0 : -1.0;
            out[i] = (float) ((a + b) * 0.3 * std::exp (-t * 9.0));
        }
        // soften the square edges a little
        float lp = 0;
        for (auto& s : out) { lp += 0.35f * (s - lp); s = lp; }
        return out;
    }

    std::vector<float> bass808 (Ctx& c, double len = 1.6)
    {
        std::vector<float> out ((size_t) (len * c.sr));
        const double f0 = noteToHz (noteNumber (c.root, 1));
        double phase = 0;
        for (size_t i = 0; i < out.size(); ++i)
        {
            const double t = (double) i / c.sr;
            const double f = f0 * std::pow (2.0, std::exp (-t * 60.0));   // short downward glide into the note
            phase += twoPi * f / c.sr;
            out[i] = (float) std::tanh (2.5 * std::sin (phase)) * (float) std::exp (-t * 1.2) * 0.8f;
        }
        return out;
    }

    void pluckNote (Ctx& c, juce::AudioBuffer<float>& dst, int midi, int offset, double len, float gain)
    {
        // Karplus-Strong
        const int period = juce::jmax (2, (int) (c.sr / noteToHz (midi)));
        std::vector<float> ring ((size_t) period);
        for (auto& s : ring) s = c.noise();
        std::vector<float> out ((size_t) (len * c.sr));
        size_t idx = 0;
        for (size_t i = 0; i < out.size(); ++i)
        {
            const size_t next = (idx + 1) % (size_t) period;
            const float v = 0.5f * (ring[idx] + ring[next]) * 0.996f;
            out[i] = ring[idx];
            ring[idx] = v;
            idx = next;
        }
        addMono (dst, out, offset, gain);
    }

    void sawNote (Ctx& c, juce::AudioBuffer<float>& dst, int midi, int offset, double len, float gain, double detuneCents, double attack, double release)
    {
        const double f = noteToHz (midi) * std::pow (2.0, detuneCents / 1200.0);
        const int n = (int) (len * c.sr);
        double phase = c.rng.nextDouble();
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / c.sr;
            phase += f / c.sr;
            phase -= std::floor (phase);
            const double saw = 2.0 * phase - 1.0;
            double env = juce::jmin (1.0, t / juce::jmax (0.001, attack));
            if (t > len - release)
                env *= juce::jmax (0.0, (len - t) / release);
            const int j = offset + i;
            if (j >= 0 && j < dst.getNumSamples())
            {
                dst.addSample (0, j, (float) (saw * env) * gain * (detuneCents < 0 ? 1.0f : 0.6f));
                dst.addSample (1, j, (float) (saw * env) * gain * (detuneCents < 0 ? 0.6f : 1.0f));
            }
        }
    }

    void lowpass (juce::AudioBuffer<float>& b, double cutoff, double sr)
    {
        const float a = (float) (1.0 - std::exp (-twoPi * cutoff / sr));
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            float z = 0;
            auto* d = b.getWritePointer (ch);
            for (int i = 0; i < b.getNumSamples(); ++i) { z += a * (d[i] - z); d[i] = z; }
        }
    }

    juce::AudioBuffer<float> pad (Ctx& c, double seconds)
    {
        auto b = makeBuffer (c, seconds);
        for (int degree : { 0, 2, 4, 7 })
        {
            const int midi = c.scaleNote (degree, 3);
            for (double det : { -9.0, 0.0, 8.0 })
                sawNote (c, b, midi, 0, seconds, 0.08f, det, seconds * 0.25, seconds * 0.3);
        }
        lowpass (b, c.has ("dark") ? 600.0 : c.has ("bright") ? 4500.0 : 1600.0, c.sr);
        lowpass (b, 5000.0, c.sr);
        return b;
    }

    juce::AudioBuffer<float> vocal (Ctx& c, double seconds)
    {
        auto b = makeBuffer (c, seconds);
        const bool male = c.hasAny ({ "male", "man", "baritone" }) && ! c.has ("female");
        const bool chop = c.hasAny ({ "chop", "stutter", "cut" });
        const int octave = male ? 3 : 4;

        // vowel formants (a, o, ee)
        const double formants[3][3] = { { 800, 1150, 2900 }, { 450, 800, 2830 }, { 270, 2290, 3010 } };
        const int vowel = c.has ("ooh") || c.has ("oh") ? 1 : c.has ("ee") ? 2 : 0;

        // source: vibrato saw following a short scale phrase
        std::vector<float> src ((size_t) b.getNumSamples());
        const double stepSeconds = chop ? 60.0 / c.bpm / 4.0 : seconds / 4.0;
        double phase = 0;
        int degree = 0;
        for (size_t i = 0; i < src.size(); ++i)
        {
            const double t = (double) i / c.sr;
            const int step = (int) (t / stepSeconds);
            if (i == 0 || (int) ((t - 1.0 / c.sr) / stepSeconds) != step)
                degree = juce::jlimit (-2, 7, degree + c.rng.nextInt (5) - 2);
            const double f = noteToHz (c.scaleNote (degree, octave)) * (1.0 + 0.006 * std::sin (twoPi * 5.5 * t));
            phase += f / c.sr;
            phase -= std::floor (phase);
            double gate = 1.0;
            if (chop)
            {
                const double inStep = std::fmod (t, stepSeconds) / stepSeconds;
                gate = (inStep < 0.7 && (step % 3 != 2)) ? 1.0 : 0.0;
            }
            src[i] = (float) ((2.0 * phase - 1.0) * gate);
        }

        std::vector<float> out (src.size(), 0.0f);
        for (int fIdx = 0; fIdx < 3; ++fIdx)
        {
            auto coeffs = juce::dsp::IIR::Coefficients<float>::makeBandPass (c.sr, (float) formants[vowel][fIdx], 6.0f);
            juce::dsp::IIR::Filter<float> filter (coeffs);
            const float g = fIdx == 0 ? 1.0f : fIdx == 1 ? 0.6f : 0.3f;
            for (size_t i = 0; i < src.size(); ++i)
                out[i] += filter.processSample (src[i]) * g;
        }
        // smooth gate clicks
        float env = 0;
        for (size_t i = 0; i < src.size(); ++i)
        {
            const float target = std::abs (src[i]) > 0.0f ? 1.0f : 0.0f;
            env += 0.004f * (target - env);
            out[i] *= juce::jmax (env, chop ? 0.0f : 1.0f);
        }
        addMono (b, out, 0, 0.8f);
        return b;
    }

    juce::AudioBuffer<float> guitar (Ctx& c, double seconds)
    {
        auto b = makeBuffer (c, seconds);
        const double step = juce::jmax (0.12, seconds / 8.0);
        int k = 0;
        for (double t = 0; t < seconds - 0.2; t += step, ++k)
            pluckNote (c, b, c.scaleNote (std::array<int, 4> { 0, 2, 4, 7 }[(size_t) (k % 4)], 3), (int) (t * c.sr), seconds - t, 0.5f);
        return b;
    }

    juce::AudioBuffer<float> melody (Ctx& c, double seconds)
    {
        auto b = makeBuffer (c, seconds);
        const double eighth = 60.0 / c.bpm / 2.0;
        int degree = 0;
        for (double t = 0; t < seconds - 0.05; t += eighth)
        {
            if (c.rng.nextFloat() < 0.2f)
                continue;   // rests
            degree = juce::jlimit (-3, 9, degree + c.rng.nextInt (5) - 2);
            const int midi = c.scaleNote (degree, 4);
            juce::AudioBuffer<float> note (2, (int) (eighth * 1.8 * c.sr));
            note.clear();
            sawNote (c, note, midi, 0, eighth * 1.8, 0.25f, -4.0, 0.004, eighth);
            sawNote (c, note, midi, 0, eighth * 1.8, 0.25f, 4.0, 0.004, eighth);
            lowpass (note, 2200.0, c.sr);
            for (int ch = 0; ch < 2; ++ch)
            {
                const int off = (int) (t * c.sr);
                const int n = juce::jmin (note.getNumSamples(), b.getNumSamples() - off);
                if (n > 0)
                    b.addFrom (ch, off, note, ch, 0, n);
            }
        }
        return b;
    }

    juce::AudioBuffer<float> riser (Ctx& c, double seconds)
    {
        auto b = makeBuffer (c, seconds);
        float lp = 0;
        double phase = 0;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double t = (double) i / c.sr;
            const double x = t / seconds;
            const float a = (float) (0.01 + 0.4 * x * x);
            lp += a * (c.noise() - lp);
            phase += twoPi * (200.0 + 1800.0 * x * x) / c.sr;
            const float s = (float) ((lp * 0.6 + std::sin (phase) * 0.25) * x);
            b.setSample (0, i, s);
            b.setSample (1, i, s);
        }
        return b;
    }

    juce::AudioBuffer<float> impact (Ctx& c)
    {
        auto b = makeBuffer (c, 2.5);
        double phase = 0;
        float lp = 0;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double t = (double) i / c.sr;
            phase += twoPi * (30.0 + 90.0 * std::exp (-t * 8.0)) / c.sr;
            lp += 0.08f * (c.noise() - lp);
            const float s = (float) (std::sin (phase) * std::exp (-t * 2.0) + lp * std::exp (-t * 4.0) * 1.5);
            b.setSample (0, i, s);
            b.setSample (1, i, s);
        }
        return b;
    }

    juce::AudioBuffer<float> percussionLoop (Ctx& c, int bars)
    {
        const double beat = 60.0 / c.bpm;
        auto b = makeBuffer (c, beat * 4.0 * bars);
        const bool trap = c.hasAny ({ "trap", "drill", "808" });
        const bool cow = c.has ("cowbell");
        auto k = kick (c), s = c.has ("clap") ? clap (c) : snare (c), h = hat (c, false), cb = cowbell (c);

        for (int bar = 0; bar < bars; ++bar)
        {
            const double barStart = bar * 4.0 * beat;
            auto at = [&] (double beats) { return (int) ((barStart + beats * beat) * c.sr); };
            addMono (b, k, at (0.0));
            addMono (b, k, at (trap ? 1.75 : 2.0), 0.9f);
            if (c.rng.nextBool()) addMono (b, k, at (trap ? 2.5 : 2.75), 0.7f);
            if (trap) addMono (b, s, at (2.0));
            else    { addMono (b, s, at (1.0)); addMono (b, s, at (3.0)); }
            for (int i = 0; i < 8; ++i)
            {
                addMono (b, h, at (i * 0.5), 0.6f);
                if (trap && c.rng.nextFloat() < 0.2f)   // hat roll
                    for (int r = 1; r < 4; ++r)
                        addMono (b, h, at (i * 0.5 + r * 0.125), 0.35f);
            }
            if (cow)
                for (double p : { 0.0, 0.75, 1.5, 2.5, 3.25 })
                    addMono (b, cb, at (p), 0.5f);
        }
        return b;
    }

    juce::AudioBuffer<float> fromMono (const Ctx& c, const std::vector<float>& v)
    {
        juce::AudioBuffer<float> b (2, juce::jmax (1, (int) v.size()));
        b.clear();
        addMono (b, v, 0);
        juce::ignoreUnused (c);
        return b;
    }

    void normalise (juce::AudioBuffer<float>& b)
    {
        const float peak = b.getMagnitude (0, b.getNumSamples());
        if (peak > 1.0e-6f)
            b.applyGain (0.891f / peak);   // -1 dBFS
        const int fade = juce::jmin (b.getNumSamples() / 2, 96);
        b.applyGainRamp (b.getNumSamples() - fade, fade, 1.0f, 0.0f);
    }
}

int noteNumber (int pitchClass, int octave) { return 12 * (octave + 1) + juce::jlimit (0, 11, pitchClass); }
double noteToHz (double midiNote)          { return 440.0 * std::pow (2.0, (midiNote - 69.0) / 12.0); }

Rendered render (const GenerationRequest& request, juce::int64 seed, double sampleRate)
{
    Ctx c;
    c.sr = sampleRate;
    c.rng.setSeed (seed);
    c.prompt = (request.userPrompt + " " + request.expandedPrompt).toLowerCase();
    c.bpm = request.bpm.value_or (c.hasAny ({ "trap", "drill" }) ? 140.0 : 120.0);
    c.bpm = juce::jlimit (60.0, 200.0, c.bpm);
    if (request.key.isNotEmpty())
    {
        for (int pc = 0; pc < 12; ++pc)
            if (pitchClassName (pc).equalsIgnoreCase (request.key))
                c.root = pc;
        c.minor = ! request.mode.equalsIgnoreCase ("major");
    }

    const double bar = 4.0 * 60.0 / c.bpm;
    Rendered r;
    r.sampleRate = sampleRate;

    // Keyword first, then generation-type default.
    const auto t = request.type;
    if (c.hasAny ({ "808", "sub bass", "bass" }) && t != GenerationType::Loop)
        { r.audio = fromMono (c, bass808 (c)); r.description = "808 bass"; }
    else if (c.has ("cowbell") && t != GenerationType::Loop)
        { r.audio = fromMono (c, cowbell (c)); r.description = "cowbell"; }
    else if (c.has ("kick") && t != GenerationType::Loop)
        { r.audio = fromMono (c, kick (c)); r.description = "kick"; }
    else if (c.has ("snare") && t != GenerationType::Loop)
        { r.audio = fromMono (c, snare (c)); r.description = "snare"; }
    else if (c.hasAny ({ "hat", "hihat", "hi-hat" }) && t != GenerationType::Loop)
        { r.audio = fromMono (c, hat (c, c.has ("open"))); r.description = "hi-hat"; }
    else if (c.has ("clap") && t != GenerationType::Loop)
        { r.audio = fromMono (c, clap (c)); r.description = "clap"; }
    else if (t == GenerationType::Vocal || c.hasAny ({ "vocal", "voice", "choir", "vox" }))
        { r.audio = vocal (c, c.hasAny ({ "chop", "stutter" }) ? bar : 2.5); r.description = "formant vocal"; }
    else if (c.hasAny ({ "guitar", "pluck", "harp" }))
        { r.audio = guitar (c, t == GenerationType::Loop ? bar * 2.0 : 2.5); r.description = "plucked string"; }
    else if (t == GenerationType::FX || c.hasAny ({ "riser", "sweep", "whoosh", "impact", "boom" }))
        { if (c.hasAny ({ "impact", "boom", "hit", "downlifter" })) { r.audio = impact (c); r.description = "impact"; }
          else { r.audio = riser (c, 4.0); r.description = "riser"; } }
    else if (t == GenerationType::Texture || c.hasAny ({ "pad", "ambient", "texture", "atmosphere", "drone" }))
        { r.audio = pad (c, 6.0); r.description = "pad texture"; }
    else if (t == GenerationType::Melody || t == GenerationType::Instrument || c.hasAny ({ "melody", "synth", "lead", "keys", "piano" }))
        { r.audio = melody (c, bar * 2.0); r.description = "synth melody"; }
    else if (t == GenerationType::Loop || t == GenerationType::Percussion || c.has ("loop"))
        { r.audio = percussionLoop (c, 2); r.description = "percussion loop"; }
    else
        { r.audio = fromMono (c, kick (c)); r.description = "kick"; }

    // Modifiers
    if (c.hasAny ({ "distort", "dirty", "saturat", "crushed" }))
        for (int ch = 0; ch < r.audio.getNumChannels(); ++ch)
            for (int i = 0; i < r.audio.getNumSamples(); ++i)
                r.audio.setSample (ch, i, std::tanh (r.audio.getSample (ch, i) * 4.0f));

    if (c.has ("dark") && r.description != "pad texture")
        lowpass (r.audio, 1500.0, c.sr);

    if (c.hasAny ({ "wide", "stereo", "spacious" }))
    {
        const int delay = (int) (0.012 * c.sr);
        juce::AudioBuffer<float> copy (r.audio);
        for (int i = 0; i < r.audio.getNumSamples(); ++i)
            r.audio.setSample (1, i, 0.6f * copy.getSample (1, i) + 0.4f * (i >= delay ? copy.getSample (0, i - delay) : 0.0f));
    }

    if (c.has ("reverse"))
        r.audio.reverse (0, r.audio.getNumSamples());

    if (request.durationSeconds.has_value())
    {
        const int maxLen = (int) (juce::jlimit (0.1, 30.0, *request.durationSeconds) * c.sr);
        if (maxLen < r.audio.getNumSamples())
            r.audio.setSize (2, maxLen, true, true, false);
    }
    else if (c.has ("short") && r.audio.getNumSamples() > (int) (0.2 * c.sr))
    {
        r.audio.setSize (2, r.audio.getNumSamples() / 2, true, true, false);
    }

    normalise (r.audio);
    return r;
}

bool writeWav (const juce::AudioBuffer<float>& audio, double sampleRate, const juce::File& file)
{
    file.deleteFile();
    auto stream = std::make_unique<juce::FileOutputStream> (file);
    if (! stream->openedOk())
        return false;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> out (std::move (stream));
    auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions{}
                                                .withSampleRate (sampleRate)
                                                .withNumChannels (audio.getNumChannels())
                                                .withBitsPerSample (24));
    if (writer == nullptr)
        return false;
    const bool ok = writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    writer.reset();
    return ok;
}
} // namespace context::synth
