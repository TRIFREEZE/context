#include "TestSupport.h"
#include "Audio/ContextRecorder.h"
#include "Session/ContextCache.h"

namespace context::test
{
/** Simulates a host: feeds blocks whose sample values encode their absolute index,
    with a play-head that advances exactly like a DAW's. */
struct FakeHost
{
    double sampleRate = 48000.0;
    double bpm = 144.0;
    double ppq = 0.0;
    bool playing = true;
    bool reportPosition = true;
    bool reportTempo = true;
    musical::TimeSignature ts {};
    juce::int64 absolute = 0;   // absolute sample index of the next block start
    bool silent = false;

    static float valueFor (juce::int64 index) { return (float) ((index % 1000000) + 1) * 1.0e-7f; }

    void run (ContextRecorder& rec, int blockSize, int blocks)
    {
        juce::AudioBuffer<float> b (2, blockSize);
        for (int n = 0; n < blocks; ++n)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const float v = silent ? 0.0f : valueFor (absolute + i);
                b.setSample (0, i, v);
                b.setSample (1, i, -v);
            }
            ContextRecorder::HostPosition pos;
            pos.playingKnown = true;
            pos.playing = playing;
            if (reportTempo) pos.bpm = bpm;
            if (reportPosition) pos.ppq = ppq;
            pos.signature = ts;
            rec.process (b.getArrayOfReadPointers(), 2, blockSize, pos);
            absolute += blockSize;
            if (playing)
                ppq += blockSize * bpm / (60.0 * sampleRate);
        }
    }
};

class ContextCaptureTests final : public juce::UnitTest
{
public:
    ContextCaptureTests() : juce::UnitTest ("Context capture", "V2") {}

    void runTest() override
    {
        beginTest ("Host-synced capture starts on the bar line and stops after exactly 8 bars");
        for (int block : { 64, 512, 1000, 4096 })
        {
            ContextRecorder rec;
            FakeHost host;
            host.ppq = 0.37;   // mid-bar: must wait for bar 2 (ppq 4)
            expect (rec.arm ({ 120.0, {} }, host.sampleRate));
            expect (rec.getState() == ContextRecorder::State::Armed);

            const double samplesPerQuarter = host.sampleRate * 60.0 / host.bpm;
            const auto expectedStart = (juce::int64) std::llround ((4.0 - 0.37) * samplesPerQuarter);
            const auto expectedLength = musical::samplesForBars (8, { 144.0, {} }, host.sampleRate);
            expectEquals (expectedLength, (juce::int64) 640000);

            // Run in blocks until the recorder completes; it must complete in the block containing the last sample.
            int guard = 0;
            while (rec.getState() != ContextRecorder::State::Complete && guard++ < 100000)
            {
                const auto before = host.absolute;
                host.run (rec, block, 1);
                if (rec.getState() == ContextRecorder::State::Complete)
                {
                    const auto lastSample = expectedStart + expectedLength - 1;
                    expect (lastSample >= before && lastSample < host.absolute, "completed in the right block");
                }
            }
            expect (rec.getState() == ContextRecorder::State::Complete);
            expect (rec.getStartMode() == ContextRecorder::StartMode::HostSync);
            expectEquals (rec.getBarsRecorded(), 8);
            host.run (rec, block, 3);   // more audio after completion must be ignored

            auto cap = rec.takeResult();
            expect (cap.has_value());
            if (cap)
            {
                expectEquals ((juce::int64) cap->audio.getNumSamples(), expectedLength);
                expectEquals (cap->audio.getSample (0, 0), FakeHost::valueFor (expectedStart), "first sample is the bar line (block " + juce::String (block) + ")");
                expectEquals (cap->audio.getSample (0, (int) expectedLength - 1), FakeHost::valueFor (expectedStart + expectedLength - 1));
                expectEquals (cap->audio.getSample (1, 100), -FakeHost::valueFor (expectedStart + 100));
                expectEquals (cap->tempo.bpm, 144.0);
                expectEquals (cap->startPpq, 4.0);
                expectEquals (cap->bars, 8);
            }
            expect (rec.getState() == ContextRecorder::State::Idle);
        }

        beginTest ("Starts immediately when playback begins exactly on a bar line; 3/4 and 6/8 lengths");
        {
            for (auto ts : { musical::TimeSignature { 4, 4 }, musical::TimeSignature { 3, 4 }, musical::TimeSignature { 6, 8 } })
            {
                ContextRecorder rec;
                FakeHost host;
                host.ts = ts;
                host.bpm = 120.0;
                host.ppq = 0.0;
                rec.arm ({ 120.0, ts }, host.sampleRate);
                host.run (rec, 512, 1);
                expect (rec.getState() == ContextRecorder::State::Recording);
                host.run (rec, 512, 3000);
                auto cap = rec.takeResult();
                expect (cap.has_value());
                if (cap)
                {
                    expectEquals ((juce::int64) cap->audio.getNumSamples(), musical::samplesForBars (8, { 120.0, ts }, 48000.0));
                    expectEquals (cap->audio.getSample (0, 0), FakeHost::valueFor (0));
                }
            }
        }

        beginTest ("Waits for the host to play; fails if playback stops or tempo changes mid-capture");
        {
            ContextRecorder rec;
            FakeHost host;
            host.playing = false;
            rec.arm ({ 120.0, {} }, host.sampleRate);
            host.run (rec, 512, 50);
            expect (rec.getState() == ContextRecorder::State::Armed, "must not start while stopped");
            host.playing = true;
            host.run (rec, 512, 200);
            expect (rec.getState() == ContextRecorder::State::Recording);
            host.playing = false;
            host.run (rec, 512, 1);
            expect (rec.getState() == ContextRecorder::State::Failed);
            expect (rec.getFailure() == ContextRecorder::Failure::TransportStopped);
            expect (ContextRecorder::describe (rec.getFailure()).isNotEmpty());
            rec.acknowledgeFailure();
            expect (rec.getState() == ContextRecorder::State::Idle);

            ContextRecorder rec2;
            FakeHost h2;
            rec2.arm ({ 120.0, {} }, h2.sampleRate);
            h2.run (rec2, 512, 100);
            h2.bpm = 150.0;
            h2.run (rec2, 512, 1);
            expect (rec2.getFailure() == ContextRecorder::Failure::TempoChanged);
        }

        beginTest ("Fallbacks: host tempo without position, and no host tempo at all (manual tempo)");
        {
            ContextRecorder rec;
            FakeHost host;
            host.reportPosition = false;
            host.silent = true;
            rec.arm ({ 100.0, {} }, host.sampleRate);
            host.run (rec, 256, 20);
            expect (rec.getState() == ContextRecorder::State::Armed, "waits for audio");
            const auto firstLoud = host.absolute;
            host.silent = false;
            host.run (rec, 256, 4000);
            expect (rec.getStartMode() == ContextRecorder::StartMode::HostTempo);
            auto cap = rec.takeResult();
            expect (cap.has_value());
            if (cap)
            {
                expectEquals ((juce::int64) cap->audio.getNumSamples(), musical::samplesForBars (8, { 144.0, {} }, 48000.0), "host tempo wins over fallback");
                // The first sample above the -60 dBFS trigger (the test signal ramps up from silence).
                const auto trigger = juce::jmax (firstLoud, (juce::int64) 10000);
                expectEquals (cap->audio.getSample (0, 0), FakeHost::valueFor (trigger));
            }

            ContextRecorder manual;
            FakeHost h2;
            h2.reportPosition = false;
            h2.reportTempo = false;
            manual.arm ({ 90.0, {} }, h2.sampleRate);
            h2.run (manual, 512, 3000);
            expect (manual.getStartMode() == ContextRecorder::StartMode::Manual);
            auto cap2 = manual.takeResult();
            expect (cap2 && cap2->audio.getNumSamples() == musical::samplesForBars (8, { 90.0, {} }, 48000.0));
        }

        beginTest ("Cancel while armed or recording; tempo too slow for the buffer");
        {
            ContextRecorder rec;
            FakeHost host;
            host.playing = false;
            rec.arm ({ 120.0, {} }, host.sampleRate);
            rec.cancel();
            expect (rec.getState() == ContextRecorder::State::Idle);

            host.playing = true;
            rec.arm ({ 120.0, {} }, host.sampleRate);
            host.run (rec, 512, 10);
            expect (rec.getState() == ContextRecorder::State::Recording);
            rec.cancel();
            expect (rec.getState() == ContextRecorder::State::Cancelling);
            expect (! rec.arm ({ 120.0, {} }, host.sampleRate), "can't re-arm until the audio thread let go");
            host.run (rec, 512, 1);
            expect (rec.getState() == ContextRecorder::State::Idle);
            expect (rec.arm ({ 120.0, {} }, host.sampleRate));

            ContextRecorder slow;
            FakeHost h2;
            h2.bpm = 45.0;   // armed expecting 140: buffer sized for 70 BPM
            slow.arm ({ 140.0, {} }, h2.sampleRate);
            h2.run (slow, 512, 400);
            expect (slow.getState() == ContextRecorder::State::Failed && slow.getFailure() == ContextRecorder::Failure::TooLong);
        }

        beginTest ("Context cache: store, load, mismatch, missing, prune");
        {
            ContextCache cache (scratchDir ("ctxcache"));
            juce::AudioBuffer<float> audio (2, 48000);
            for (int i = 0; i < audio.getNumSamples(); ++i) { audio.setSample (0, i, 0.25f); audio.setSample (1, i, -0.25f); }
            ContextMeta m;
            m.id = juce::Uuid().toDashedString();
            m.tempo = { 120.0, {} };
            m.bars = 8;
            m.sampleRate = 48000.0;
            m.lengthSamples = 48000;
            m.captured = juce::Time::getCurrentTime();
            m.startMode = "host";
            juce::String err;
            expect (cache.store (audio, m, err), err);
            auto loaded = cache.load (m);
            expect (loaded.ok(), loaded.error);
            if (loaded.ok())
                expectWithinAbsoluteError (loaded.asset->audio.getSample (0, 1000), 0.25f, 1.0e-5f);

            auto round = ContextMeta::fromVar (juce::JSON::parse (juce::JSON::toString (m.toVar())));
            expect (round.has_value() && round->id == m.id && round->tempo.bpm == 120.0 && round->bars == 8);

            auto wrongLen = m;
            wrongLen.lengthSamples = 47000;
            expect (! cache.load (wrongLen).ok());
            auto missing = m;
            missing.id = juce::Uuid().toDashedString();
            const auto missingResult = cache.load (missing);
            expect (! missingResult.ok() && missingResult.error.contains ("Capture it again"));

            auto evil = m.toVar();
            evil.getDynamicObject()->setProperty ("id", "../../etc/passwd");
            expect (! ContextMeta::fromVar (evil).has_value(), "path traversal ids rejected");
            expect (! ContextMeta::fromVar (juce::var ("junk")).has_value());

            for (int i = 0; i < 4; ++i)
            {
                auto extra = m;
                extra.id = juce::Uuid().toDashedString();
                cache.store (audio, extra, err);
            }
            cache.prune (2, (juce::int64) 1 << 40, m.id);
            const auto left = cache.folder().findChildFiles (juce::File::findFiles, false, "context-*.wav").size();
            expect (left <= 3, "pruned to newest 2 + protected (" + juce::String (left) + ")");
            expect (cache.fileFor (m.id).existsAsFile(), "protected capture kept");
        }
    }
};

static ContextCaptureTests contextCaptureTests;
} // namespace context::test
