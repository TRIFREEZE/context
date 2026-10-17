#include "TestSupport.h"
#include "Audio/TimeFit.h"
#include "Audio/TimeStretch/StretchEngines.h"

namespace context::test
{
namespace
{
    constexpr double sr = 48000.0;

    juce::AudioBuffer<float> tone (double freq, int n, int channels = 2, float amp = 0.4f)
    {
        juce::AudioBuffer<float> b (channels, n);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < n; ++i)
                b.setSample (ch, i, amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / sr));
        return b;
    }

    juce::AudioBuffer<float> drums (int n, double interval = 0.25)
    {
        juce::AudioBuffer<float> b (2, n);
        b.clear();
        juce::Random rng (3);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr, beat = std::fmod (t, interval);
            const float v = (float) ((std::sin (2.0 * juce::MathConstants<double>::pi * 60.0 * beat) * 0.9 + (rng.nextFloat() * 2 - 1) * 0.5) * std::exp (-beat * 40.0));
            b.setSample (0, i, v);
            b.setSample (1, i, v);
        }
        return b;
    }

    double freqOf (const juce::AudioBuffer<float>& b, int start, int len)
    {
        int first = -1, last = -1, count = 0;
        for (int i = start + 1; i < start + len; ++i)
            if (b.getSample (0, i - 1) < 0.0f && b.getSample (0, i) >= 0.0f)
            {
                if (first < 0) first = i;
                last = i;
                ++count;
            }
        return count > 1 ? (count - 1) * sr / (last - first) : 0.0;
    }

    double rmsOf (const juce::AudioBuffer<float>& b, int start, int len)
    {
        double s = 0.0;
        for (int i = start; i < start + len; ++i) s += (double) b.getSample (0, i) * b.getSample (0, i);
        return std::sqrt (s / len);
    }

    double diff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        const int n = juce::jmin (a.getNumSamples(), b.getNumSamples());
        double s = 0.0, r = 1.0e-12;
        for (int i = 0; i < n; ++i) { const double d = a.getSample (0, i) - b.getSample (0, i); s += d * d; r += (double) a.getSample (0, i) * a.getSample (0, i); }
        return std::sqrt (s / r);
    }

    AudioAsset::Ptr asset (juce::AudioBuffer<float> audio, double rate = sr, juce::String name = "src.wav")
    {
        auto a = std::make_shared<AudioAsset>();
        a->id = "test";
        a->audio = std::move (audio);
        a->sampleRate = rate;
        a->displayName = name;
        return a;
    }
}

class StretchTests final : public juce::UnitTest
{
public:
    StretchTests() : juce::UnitTest ("Time stretch and fit", "V2") {}

    juce::AudioBuffer<float> run (stretch::Algorithm a, const juce::AudioBuffer<float>& in, double ratio, double pitch = 0.0)
    {
        auto e = stretch::createEngine (a);
        e->prepare (sr, in.getNumChannels());
        e->setRatio (ratio);
        e->setPitchSemitones (pitch);
        return e->process (in);
    }

    void runTest() override
    {
        using stretch::Algorithm;
        const Algorithm engines[] = { Algorithm::Percussive, Algorithm::Monophonic, Algorithm::Polyphonic, Algorithm::Texture, Algorithm::Repitch };

        beginTest ("Every engine returns exactly round(length * ratio) samples");
        {
            const auto in = tone (330.0, 30011);
            for (auto a : engines)
                for (double r : { 0.5, 0.9, 1.0, 1.07, 1.5, 2.0 })
                {
                    const auto out = run (a, in, r);
                    expectEquals ((juce::int64) out.getNumSamples(), std::llround (30011 * r), stretch::toLabel (a) + " @ " + juce::String (r));
                    expectEquals (out.getNumChannels(), 2);
                }
        }

        beginTest ("Time-stretching keeps pitch and level; Repitch moves pitch with speed");
        {
            const auto in = tone (440.0, (int) (sr * 2.0));
            for (auto a : { Algorithm::Monophonic, Algorithm::Polyphonic, Algorithm::Texture, Algorithm::Percussive })
            {
                const auto out = run (a, in, 1.25);
                const int mid = out.getNumSamples() / 4, len = out.getNumSamples() / 2;
                expectWithinAbsoluteError (freqOf (out, mid, len), 440.0, 440.0 * 0.01, stretch::toLabel (a) + " pitch");
                expectWithinAbsoluteError (juce::Decibels::gainToDecibels (rmsOf (out, mid, len) / rmsOf (in, mid, len)), 0.0, 1.5, stretch::toLabel (a) + " level");
            }
            const auto tape = run (Algorithm::Repitch, in, 1.25);
            expectWithinAbsoluteError (freqOf (tape, 20000, 60000), 352.0, 2.0, "repitch lowers pitch by the stretch");
        }

        beginTest ("Pitch shift without changing length");
        {
            const auto in = tone (330.0, (int) (sr * 1.5));
            for (auto a : { Algorithm::Polyphonic, Algorithm::Monophonic })
            {
                const auto up = run (a, in, 1.0, 12.0);
                expectEquals (up.getNumSamples(), in.getNumSamples());
                expectWithinAbsoluteError (freqOf (up, 10000, 50000), 660.0, 660.0 * 0.012, stretch::toLabel (a) + " +12 st");
                const auto down = run (a, in, 1.0, -7.0);
                expectWithinAbsoluteError (freqOf (down, 10000, 50000), 330.0 * std::pow (2.0, -7.0 / 12.0), 3.0, stretch::toLabel (a) + " -7 st");
            }
        }

        beginTest ("Percussive mode keeps attacks on the scaled grid; modes are genuinely different");
        {
            const auto in = drums ((int) (sr * 2.0));
            const auto onsets = stretch::detectOnsets (in, sr);
            expect (onsets.size() >= 7 && onsets.size() <= 9, "8 hits detected, got " + juce::String ((int) onsets.size()));
            const double ratio = 1.5;
            const auto perc = run (Algorithm::Percussive, in, ratio);
            const auto out = stretch::detectOnsets (perc, sr);
            juce::String positions;
            for (auto o : onsets) positions << o << " ";
            positions << "| ";
            for (auto o : out) positions << o << " ";
            for (auto o : onsets)
            {
                int nearest = -1000000;
                for (auto q : out) if (std::abs (q - o * ratio) < std::abs (nearest - o * ratio)) nearest = q;
                expect (std::abs (nearest - o * ratio) < sr * 0.003, "hit at " + juce::String (o) + " lands at its scaled position: " + positions);
            }
            // Attack peak preserved by Percussive (copied), typically reduced by the vocoder.
            const float inPeak = in.getMagnitude (0, 0, 4800);
            expect (perc.getMagnitude (0, 0, 4800) > inPeak * 0.97f, "percussive keeps the first attack's peak");

            const auto poly = run (Algorithm::Polyphonic, in, ratio), mono = run (Algorithm::Monophonic, in, ratio),
                       tex = run (Algorithm::Texture, in, ratio), rep = run (Algorithm::Repitch, in, ratio);
            for (auto* other : { &poly, &mono, &tex, &rep })
                expect (diff (perc, *other) > 0.05, "different algorithms give different results");
            expect (diff (poly, tex) > 0.05, "polyphonic and texture differ");
        }

        beginTest ("Auto picks a sensible mode for drums, a single voice and noise");
        {
            expect (stretch::analyseMaterial (drums ((int) (sr * 3.0)), sr).suggested == Algorithm::Percussive);
            // A slow single-note melody (sine-ish with harmonics): monophonic.
            juce::AudioBuffer<float> melody (2, (int) (sr * 3.0));
            for (int i = 0; i < melody.getNumSamples(); ++i)
            {
                const double t = i / sr;
                const double f = t < 1.0 ? 220.0 : t < 2.0 ? 247.0 : 196.0;
                const float v = (float) (0.4 * std::sin (2 * juce::MathConstants<double>::pi * f * t) + 0.1 * std::sin (4 * juce::MathConstants<double>::pi * f * t));
                melody.setSample (0, i, v);
                melody.setSample (1, i, v);
            }
            const auto m = stretch::analyseMaterial (melody, sr);
            expect (m.suggested == Algorithm::Monophonic, "melody -> " + stretch::toLabel (m.suggested) + " (periodicity " + juce::String (m.periodicity, 2) + ")");
            juce::AudioBuffer<float> noise (2, (int) (sr * 3.0));
            juce::Random rng (9);
            for (int i = 0; i < noise.getNumSamples(); ++i) { noise.setSample (0, i, rng.nextFloat() * 0.2f - 0.1f); noise.setSample (1, i, rng.nextFloat() * 0.2f - 0.1f); }
            const auto n = stretch::analyseMaterial (noise, sr);
            expect (n.suggested == Algorithm::Texture, "noise -> " + stretch::toLabel (n.suggested) + " (flatness " + juce::String (n.flatness, 2) + ")");
        }

        beginTest ("Auto fit decisions");
        {
            expect (timefit::chooseMode (0.2, 8, true) == FitMode::Trim);
            expect (timefit::chooseMode (8.002, 8, false) == FitMode::Trim);
            expect (timefit::chooseMode (7.54, 8, false) == FitMode::Stretch);
            expect (timefit::chooseMode (8.4, 8, false) == FitMode::Stretch);
            expect (timefit::chooseMode (2.05, 8, false) == FitMode::Loop);
            expect (timefit::chooseMode (3.9, 8, false) == FitMode::Loop);
            expect (timefit::chooseMode (20.0, 8, false) == FitMode::Trim);
            expect (timefit::chooseMode (3.0, 8, false) == FitMode::Loop);
            expectEquals (timefit::loopUnitBars (2.05, 8), 2);
            expectEquals (timefit::loopUnitBars (3.0, 8), 0);
            expectEquals (timefit::loopUnitBars (5.9, 6), 6);
        }

        beginTest ("Fit render: every mode yields exactly N bars, at any output rate");
        {
            const musical::Tempo t144 { 144.0, {} };
            // A loop that is 7.54 bars at 144 BPM.
            const int srcLen = (int) std::llround (musical::secondsForBars (7.54, t144) * 44100.0);
            auto src = asset (drums (srcLen, 60.0 / 144.0), 44100.0);   // hits on the beat
            for (double outRate : { 44100.0, 48000.0, 96000.0 })
                for (auto mode : allFitModes)
                {
                    FitRequest req;
                    req.source = src;
                    req.tempo = t144;
                    req.bars = 8;
                    req.outputRate = outRate;
                    req.settings.mode = mode;
                    const auto r = timefit::render (req);
                    expect (r.ok(), r.error);
                    if (! r.ok()) continue;
                    expectEquals ((juce::int64) r.audio->getNumSamples(), musical::samplesForBars (8, t144, outRate),
                                  toLabel (mode) + " @ " + juce::String (outRate));
                    expectEquals (r.audio->getNumChannels(), 2);
                    expectWithinAbsoluteError (r.report.sourceBars, 7.54, 0.001);
                    if (mode == FitMode::Auto)
                        expect (r.report.modeUsed == FitMode::Stretch && r.report.engineUsed, "auto stretches a 7.54-bar loop");
                }
            FitRequest req;
            req.source = src;
            req.tempo = t144;
            req.bars = 8;
            req.outputRate = 48000.0;
            req.settings.mode = FitMode::Stretch;
            const auto r = timefit::render (req);
            expectWithinAbsoluteError (r.report.stretchPercent, 100.0 * (8.0 / 7.54 - 1.0), 0.05, "+6.1% stretch reported");
        }

        beginTest ("Loop: a 2-bar loop fills 8 bars, each repeat exactly on its bar line");
        {
            const musical::Tempo t { 120.0, {} };
            const int twoBars = (int) musical::samplesForBars (2, t, sr);
            auto cell = drums (twoBars, 0.5);
            cell.setSample (0, 0, 0.77f);   // marker at the start of the loop
            cell.setSample (1, 0, 0.77f);
            FitRequest req;
            req.source = asset (cell);
            req.tempo = t;
            req.bars = 8;
            req.outputRate = sr;
            const auto r = timefit::render (req);
            expect (r.ok());
            expect (r.report.modeUsed == FitMode::Loop);
            expectEquals (r.report.loopRepeats, 4);
            expect (! r.report.engineUsed, "already on the grid: no stretching");
            for (int k = 0; k < 4; ++k)
                expectEquals (r.audio->getSample (0, (int) musical::samplesForBars (2.0 * k, t, sr)), 0.77f);
        }

        beginTest ("One-shots sit on bar 1 padded; long sources are trimmed with a fade; mono becomes equal stereo");
        {
            const musical::Tempo t { 140.0, {} };
            auto kick = tone (60.0, (int) (sr * 0.3), 1, 0.9f);
            FitRequest req;
            req.source = asset (kick);
            req.tempo = t;
            req.bars = 4;
            req.outputRate = sr;
            const auto r = timefit::render (req);
            expect (r.report.modeUsed == FitMode::Trim && ! r.report.engineUsed);
            expectEquals (r.audio->getSample (0, 1000), kick.getSample (0, 1000));
            expectEquals (r.audio->getSample (1, 1000), kick.getSample (0, 1000));
            expectEquals (r.audio->getMagnitude (0, (int) (sr * 0.31), 1000), 0.0f);

            auto longSrc = tone (220.0, (int) (sr * 40.0));
            req.source = asset (longSrc);
            const auto t2 = timefit::render (req);
            expect (t2.report.modeUsed == FitMode::Trim);
            const int n = t2.audio->getNumSamples();
            expect (std::abs (t2.audio->getSample (0, n - 1)) < 0.01f, "faded at the cut");

            req.settings.mode = FitMode::Repitch;
            req.source = asset (tone (220.0, (int) (musical::secondsForBars (4.4, t) * sr)));
            const auto rp = timefit::render (req);
            expect (rp.report.modeUsed == FitMode::Repitch);
            expect (rp.report.pitchSemitones > 1.0 && rp.report.pitchSemitones < 2.0, "4.4 bars -> 4 bars raises pitch ~1.65 st: " + juce::String (rp.report.pitchSemitones));
        }

        beginTest ("Source tempo inference and errors");
        {
            const musical::Tempo project { 144.0, {} };
            juce::String origin;
            auto named = asset (tone (100.0, 1000), sr, "guitar_130bpm.wav");
            expect (timefit::inferSourceBpm (*named, project, origin) == 130.0 && origin == "file name");
            auto gen = std::make_shared<AudioAsset> (*named);
            gen->origin = AudioAsset::Origin::Generated;
            gen->bpm = 142.0;
            expect (timefit::inferSourceBpm (*gen, project, origin) == 142.0 && origin == "generator");
            // 8 bars at 150 BPM = 12.8 s
            auto byLength = asset (tone (100.0, (int) (12.8 * sr)), sr, "loop.wav");
            auto inferred = timefit::inferSourceBpm (*byLength, project, origin);
            expect (inferred.has_value() && std::abs (*inferred - 150.0) < 0.01 && origin == "length");

            FitRequest bad;
            expect (! timefit::render (bad).ok());
            bad.source = byLength;
            bad.tempo = { 0.0, {} };
            expect (timefit::render (bad).error.contains ("tempo"));

            FitRequest cancelReq;
            cancelReq.source = byLength;
            cancelReq.tempo = project;
            cancelReq.settings.mode = FitMode::Stretch;
            cancelReq.settings.algorithm = stretch::Algorithm::Polyphonic;
            CancellationToken token;
            token.cancel();
            stretch::TimeStretchEngine::Progress prog;
            prog.cancel = &token;
            expect (! timefit::render (cancelReq, prog).ok(), "cancelled render returns nothing");

            const auto s = FitSettings::fromVar (juce::JSON::parse (R"({"mode":"loop","algorithm":"texture","pitch":99})"));
            expect (s.mode == FitMode::Loop && s.algorithm == stretch::Algorithm::Texture && s.pitchSemitones == 24.0);
            expect (FitSettings::fromVar (juce::var ("junk")) == FitSettings{});
        }
    }
};

static StretchTests stretchTests;
} // namespace context::test
