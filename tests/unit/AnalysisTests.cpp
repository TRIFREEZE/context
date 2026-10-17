#include "TestSupport.h"
#include "Audio/CaptureBuffer.h"
#include "Analysis/TrackAnalyzer.h"
#include "Analysis/AnalysisController.h"
#include "Generation/PromptBuilder.h"
#include "Generation/SynthAudio.h"
#include "Networking/AudioFileValidator.h"

namespace context::test
{
namespace
{
    constexpr double pi2 = juce::MathConstants<double>::twoPi;

    /** Chord progression in a key, with harmonics, so key detection has something real to chew on. */
    juce::AudioBuffer<float> progression (double sr, double seconds, int root, bool minor, double width = 0.0)
    {
        juce::AudioBuffer<float> b (2, (int) (sr * seconds));
        b.clear();
        // i - iv - v - i  (or I - IV - V - I), triads in octave 3-4
        const int third = minor ? 3 : 4;
        const int chords[4][3] = { { 0, third, 7 }, { 5, 5 + third, 12 }, { 7, 7 + (minor ? 3 : 4), 14 }, { 0, third, 7 } };
        const int chordLen = b.getNumSamples() / 4;
        for (int c = 0; c < 4; ++c)
            for (int note = 0; note < 3; ++note)
            {
                const double f = synth::noteToHz (synth::noteNumber (root, 3) + chords[c][note]);
                for (int i = 0; i < chordLen; ++i)
                {
                    const double t = i / sr;
                    const double env = juce::jmin (1.0, t * 50.0) * std::exp (-t * 0.8);
                    double s = 0;
                    for (int h = 1; h <= 4; ++h)
                        s += std::sin (pi2 * f * h * t + note) / (h * h);
                    const int idx = c * chordLen + i;
                    b.addSample (0, idx, (float) (s * env * 0.15 * (1.0 + width * (note - 1))));
                    b.addSample (1, idx, (float) (s * env * 0.15 * (1.0 - width * (note - 1))));
                }
            }
        return b;
    }

    juce::AudioBuffer<float> clicks (double sr, double seconds, double bpm, int perBeat)
    {
        juce::AudioBuffer<float> b (2, (int) (sr * seconds));
        b.clear();
        juce::Random rng (3);
        const double step = 60.0 / bpm / perBeat;
        for (double t = 0; t < seconds; t += step)
        {
            const int start = (int) (t * sr);
            for (int i = 0; i < (int) (0.03 * sr) && start + i < b.getNumSamples(); ++i)
            {
                const float v = (rng.nextFloat() * 2.0f - 1.0f) * (float) std::exp (-i / (0.004 * sr)) * 0.8f;
                b.setSample (0, start + i, v);
                b.setSample (1, start + i, v);
            }
        }
        return b;
    }
}

class CaptureBufferTests final : public juce::UnitTest
{
public:
    CaptureBufferTests() : juce::UnitTest ("Capture buffer", "Analysis") {}

    void runTest() override
    {
        beginTest ("Wraps, keeps newest audio in order, ignores silence");
        CaptureBuffer cap;
        cap.prepare (1000.0, 2);   // 2000 samples
        std::vector<float> l (300), r (300);
        float counter = 1.0f;
        for (int block = 0; block < 10; ++block)
        {
            for (int i = 0; i < 300; ++i) { l[(size_t) i] = counter; r[(size_t) i] = -counter; counter += 1.0f; }
            const float* chans[] = { l.data(), r.data() };
            cap.push (chans, 2, 300);
        }
        std::vector<float> silent (300, 0.0f);
        const float* silentChans[] = { silent.data(), silent.data() };
        cap.push (silentChans, 2, 300);

        auto snap = cap.snapshot();
        expectEquals (snap.audio.getNumSamples(), 2000);
        expectEquals (snap.audio.getSample (0, 1999), 3000.0f);   // newest sample last
        expectEquals (snap.audio.getSample (0, 0), 1001.0f);      // oldest retained
        expectEquals (snap.audio.getSample (1, 0), -1001.0f);
        expectWithinAbsoluteError (cap.capturedSeconds(), 2.0, 1e-9);

        beginTest ("Mono input duplicated; block longer than ring");
        cap.prepare (1500.0, 1);   // 1500-sample ring
        std::vector<float> big (2500);
        for (int i = 0; i < 2500; ++i) big[(size_t) i] = (float) i + 1.0f;
        const float* mono[] = { big.data() };
        cap.push (mono, 1, 2500);
        snap = cap.snapshot();
        expectEquals (snap.audio.getNumSamples(), 1500);
        expectEquals (snap.audio.getSample (0, 1499), 2500.0f);
        expectEquals (snap.audio.getSample (1, 1499), 2500.0f);
        expectEquals (snap.audio.getSample (0, 0), 1001.0f);

        beginTest ("NaN / null input never crashes");
        std::vector<float> nan (64, std::numeric_limits<float>::quiet_NaN());
        const float* nanChans[] = { nan.data(), nan.data() };
        cap.push (nanChans, 2, 64);
        cap.push (nullptr, 2, 64);
        const float* nulls[] = { nullptr, nullptr };
        cap.push (nulls, 2, 64);
        expectEquals (cap.snapshot().audio.getNumSamples(), 1500);

        beginTest ("Concurrent push + snapshot (audio thread never blocks)");
        cap.prepare (48000.0, 8);
        std::atomic<bool> stop { false };
        std::atomic<int> pushes { 0 };
        std::thread audio ([&]
        {
            std::vector<float> a (256, 0.25f);
            const float* ch[] = { a.data(), a.data() };
            while (! stop.load())
            {
                const auto t0 = juce::Time::getHighResolutionTicks();
                cap.push (ch, 2, 256);
                const auto us = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1e6;
                if (us > 2000.0) { stop = true; }   // a push should never take milliseconds
                ++pushes;
            }
        });
        for (int i = 0; i < 50; ++i)
            snap = cap.snapshot();
        for (int i = 0; i < 5; ++i)
            cap.prepare (i % 2 ? 44100.0 : 96000.0, 8);   // sample-rate changes while running
        stop = true;
        audio.join();
        expect (pushes.load() > 100);
    }
};

class AnalyzerTests final : public juce::UnitTest
{
public:
    AnalyzerTests() : juce::UnitTest ("Track analyzer", "Analysis") {}

    void runTest() override
    {
        beginTest ("Key detection across sample rates");
        struct K { int root; bool minor; };
        for (double sr : { 44100.0, 48000.0, 96000.0 })
            for (auto k : { K { 9, true }, K { 0, false }, K { 6, true }, K { 7, false }, K { 2, true } })
            {
                const auto a = analysis::analyze (progression (sr, 8.0, k.root, k.minor), sr, std::nullopt);
                const auto expected = pitchClassName (k.root) + (k.minor ? " Minor" : " Major");
                expect (a.valid);
                logMessage ("  " + expected + " @" + juce::String (sr) + " -> " + a.keyDisplay() + " conf " + juce::String (a.keyConfidence, 2));
                expect (a.keyKnown, "key unknown for " + expected + " @" + juce::String (sr) + " conf " + juce::String (a.keyConfidence));
                expectEquals (a.keyDisplay(), expected, "@" + juce::String (sr));
            }

        beginTest ("Silence and noise are reported honestly");
        {
            juce::AudioBuffer<float> silence (2, 48000 * 4);
            silence.clear();
            expect (! analysis::analyze (silence, 48000.0, std::nullopt).valid);

            juce::AudioBuffer<float> noise (2, 48000 * 4);
            juce::Random rng (1);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < noise.getNumSamples(); ++i)
                    noise.setSample (ch, i, (rng.nextFloat() * 2.0f - 1.0f) * 0.3f);
            const auto a = analysis::analyze (noise, 48000.0, std::nullopt);
            expect (a.valid);
            expect (! a.keyKnown, "white noise must not get a confident key: " + a.keyDisplay() + " " + juce::String (a.keyConfidence));
            expect (a.stereo == StereoWidth::Wide, "independent L/R noise is wide");
            expect (a.highs == Level::High);
        }

        beginTest ("Frequency balance: sub-heavy vs bright");
        {
            juce::AudioBuffer<float> sub (2, 44100 * 4);
            for (int i = 0; i < sub.getNumSamples(); ++i)
            {
                const float v = (float) (0.6 * std::sin (pi2 * 50.0 * i / 44100.0) + 0.05 * std::sin (pi2 * 1000.0 * i / 44100.0));
                sub.setSample (0, i, v); sub.setSample (1, i, v);
            }
            const auto a = analysis::analyze (sub, 44100.0, std::nullopt);
            expect (a.lowEnd == Level::High);
            expect (a.highs == Level::Low);
            expect (a.tonal == TonalBalance::Dark);
            expect (a.stereo == StereoWidth::Narrow, "identical channels are narrow");
            expectWithinAbsoluteError (a.correlation, 1.0, 1e-6);
        }

        beginTest ("Mono buffer input");
        {
            juce::AudioBuffer<float> mono (1, 44100 * 3);
            for (int i = 0; i < mono.getNumSamples(); ++i)
                mono.setSample (0, i, (float) (0.4 * std::sin (pi2 * 220.0 * i / 44100.0)));
            const auto a = analysis::analyze (mono, 44100.0, 128.0);
            expect (a.valid);
            expect (a.stereo == StereoWidth::Narrow);
            expectEquals (*a.bestBpm(), 128.0);
        }

        beginTest ("Transient density and tempo estimate");
        {
            const auto sparse = analysis::analyze (clicks (44100.0, 8.0, 60.0, 1), 44100.0, std::nullopt);
            expect (sparse.transients == TransientDensity::Sparse, juce::String (sparse.onsetsPerSecond));
            const auto dense = analysis::analyze (clicks (44100.0, 8.0, 140.0, 4), 44100.0, std::nullopt);
            expect (dense.transients == TransientDensity::Dense, juce::String (dense.onsetsPerSecond));
            const auto beat = analysis::analyze (clicks (48000.0, 8.0, 128.0, 1), 48000.0, std::nullopt);
            expect (beat.estimatedBpm.has_value(), "tempo estimate expected");
            if (beat.estimatedBpm)
                expectWithinAbsoluteError (*beat.estimatedBpm, 128.0, 2.5);
        }

        beginTest ("Host BPM wins; invalid host BPM ignored");
        {
            const auto a = analysis::analyze (clicks (48000.0, 6.0, 128.0, 1), 48000.0, 142.0);
            expectEquals (*a.bestBpm(), 142.0);
            const auto b = analysis::analyze (clicks (48000.0, 6.0, 128.0, 1), 48000.0, 0.0);
            expect (! b.hostBpm.has_value());
        }
    }
};

class AnalysisControllerTests final : public juce::UnitTest
{
public:
    AnalysisControllerTests() : juce::UnitTest ("Analysis controller", "Analysis") {}

    void runTest() override
    {
        beginTest ("Not enough audio, then success; rapid clicks coalesce");
        CaptureBuffer cap;
        cap.prepare (48000.0, 8);
        AnalysisController ac (cap, [] { return std::optional<double> (140.0); });

        expect (ac.requestAnalysis());
        expect (waitUntil ([&] { return ac.getState() != AnalysisController::State::Analyzing; }, 5000));
        expect (ac.getState() == AnalysisController::State::NotEnoughAudio);
        expect (ac.getMessage().contains ("Not enough audio"));

        const auto music = progression (48000.0, 6.0, 9, true);
        for (int pos = 0; pos < music.getNumSamples(); pos += 512)
        {
            const int n = juce::jmin (512, music.getNumSamples() - pos);
            const float* ch[] = { music.getReadPointer (0, pos), music.getReadPointer (1, pos) };
            cap.push (ch, 2, n);
        }

        int accepted = 0;
        for (int i = 0; i < 20; ++i)
            accepted += ac.requestAnalysis() ? 1 : 0;
        expectEquals (accepted, 1);
        expect (waitUntil ([&] { return ac.getState() == AnalysisController::State::Done; }, 10000));
        const auto latest = ac.getLatest();
        expect (latest.has_value() && latest->keyDisplay() == "A Minor");
        expectEquals (*latest->hostBpm, 140.0);

        beginTest ("Audio context WAV is small, mono, 22.05 kHz");
        auto f = scratchDir ("ctx").getChildFile ("context.wav");
        const auto payload = ac.writeAudioContext (f, 8.0);
        expect (payload.has_value());
        const auto info = AudioFileValidator::validate (f);
        expect (info.ok);
        expectEquals (info.channels, 1);
        expectEquals (info.sampleRate, 22050.0);
        expect (f.getSize() < 400 * 1024);
    }
};

class PromptBuilderTests final : public juce::UnitTest
{
public:
    PromptBuilderTests() : juce::UnitTest ("Prompt builder", "Prompt") {}

    static TrackAnalysis heavyBassTrack()
    {
        TrackAnalysis a;
        a.valid = true;
        a.hostBpm = 142.0;
        a.keyKnown = true; a.keyRoot = 6; a.minor = true; a.keyConfidence = 0.74;
        a.lowEnd = Level::High; a.mids = Level::Medium; a.highs = Level::Medium;
        a.stereo = StereoWidth::Wide; a.transients = TransientDensity::Moderate; a.tonal = TonalBalance::Balanced;
        return a;
    }

    void runTest() override
    {
        beginTest ("Vocal chop on a bass-heavy track");
        PromptBuilder::Inputs in;
        in.userPrompt = "dark distorted female vocal chop";
        in.type = GenerationType::Vocal;
        in.analysis = heavyBassTrack();
        in.hostBpm = 142.0;
        auto out = PromptBuilder::build (in);
        logMessage ("  -> " + out.expandedPrompt);
        expect (out.expandedPrompt.startsWith ("Dark distorted female vocal chop."));
        expect (out.expandedPrompt.contains ("142 BPM track in F# minor"));
        expect (out.expandedPrompt.contains ("Short rhythmic phrasing"));
        expect (out.expandedPrompt.contains ("keep unnecessary sub-bass minimal while preserving vocal body"));
        expectEquals (*out.bpm, 142.0);
        expectEquals (out.key, juce::String ("F#"));
        expectEquals (out.mode, juce::String ("minor"));

        beginTest ("User intent outranks heuristics: huge sub bass is not told to avoid low end");
        in.userPrompt = "a huge sub bass";
        in.type = GenerationType::Instrument;
        out = PromptBuilder::build (in);
        logMessage ("  -> " + out.expandedPrompt);
        expect (! out.expandedPrompt.contains ("sub-bass minimal"), out.expandedPrompt);
        expect (out.expandedPrompt.contains ("mono-compatible"));

        beginTest ("Melody on heavy-bass track gets reduced low end");
        in.userPrompt = "plucky lead melody";
        in.type = GenerationType::Melody;
        out = PromptBuilder::build (in);
        expect (out.expandedPrompt.contains ("sub-bass minimal"));
        expect (out.expandedPrompt.contains ("locked to the grid"));

        beginTest ("Explicit BPM and key in the prompt are never overridden");
        in.userPrompt = "140 BPM percussion loop in A minor";
        in.type = GenerationType::Loop;
        out = PromptBuilder::build (in);
        expectEquals (*out.bpm, 140.0);
        expect (out.key.isEmpty(), "key must not be overridden");
        expect (! out.expandedPrompt.contains ("142"), out.expandedPrompt);
        expect (! out.expandedPrompt.contains ("F#"), out.expandedPrompt);

        beginTest ("Drums don't get a key; tonal drums do");
        in.userPrompt = "short punchy trap kick";
        in.type = GenerationType::OneShot;
        out = PromptBuilder::build (in);
        expect (out.key.isEmpty());
        expect (! out.expandedPrompt.contains ("sub-bass minimal"), "a kick is allowed its low end");
        in.userPrompt = "808 with long tail";
        out = PromptBuilder::build (in);
        expectEquals (out.key, juce::String ("F#"));

        beginTest ("Expansion off: prompt untouched but structured fields still sent");
        in.userPrompt = "wide ambient synth texture";
        in.type = GenerationType::Texture;
        in.expansionEnabled = false;
        out = PromptBuilder::build (in);
        expectEquals (out.expandedPrompt, juce::String ("wide ambient synth texture"));
        expectEquals (*out.bpm, 142.0);
        expectEquals (out.key, juce::String ("F#"));

        beginTest ("Track context off: nothing from the track is used");
        PromptBuilder::Inputs plain;
        plain.userPrompt = "hard Memphis cowbell";
        plain.type = GenerationType::Percussion;
        out = PromptBuilder::build (plain);
        expect (! out.bpm.has_value() && out.key.isEmpty());
        expect (out.expandedPrompt.startsWith ("Hard Memphis cowbell."));
        expect (out.expandedPrompt.length() < 120);

        beginTest ("Uncertain key is flagged; dense groove favours smooth vocals");
        auto a = heavyBassTrack();
        a.keyConfidence = 0.55; a.transients = TransientDensity::Dense; a.lowEnd = Level::Medium;
        in = {};
        in.userPrompt = "ethereal choir";
        in.type = GenerationType::Vocal;
        in.analysis = a;
        out = PromptBuilder::build (in);
        expect (out.expandedPrompt.contains ("uncertain"));
        expect (out.expandedPrompt.contains ("smooth, sustained"));

        beginTest ("Hostile / odd prompts are safe");
        in.userPrompt = juce::String::repeatedString (juce::CharPointer_UTF8 ("\xf0\x9f\x94\xa5 "), 400);
        out = PromptBuilder::build (in);
        expect (out.expandedPrompt.length() <= PromptBuilder::maxLength);
        in.userPrompt = "";
        out = PromptBuilder::build (in);
        expect (out.expandedPrompt.isEmpty());
        expect (! PromptBuilder::detectIntent ("i am a wide pad").explicitKey);
        expect (PromptBuilder::detectIntent ("pad in f# minor").explicitKey);
        expect (PromptBuilder::detectIntent ("C major chords").explicitKey);
        expect (! PromptBuilder::detectIntent ("dark ambient vocal").explicitKey);
        expectEquals (*PromptBuilder::detectIntent ("loop at 92bpm").explicitBpm, 92.0);
    }
};

static CaptureBufferTests captureBufferTests;
static AnalyzerTests analyzerTests;
static AnalysisControllerTests analysisControllerTests;
static PromptBuilderTests promptBuilderTests;
} // namespace context::test
