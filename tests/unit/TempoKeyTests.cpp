// Setting the tempo and key yourself: half-time / double-time re-reads the captured track,
// other tempos re-fit it, the chosen key reaches the prompt, and both survive a reload.
#include "TestSupport.h"
#include "PluginProcessor.h"

namespace context::test
{
namespace
{
    struct Head final : juce::AudioPlayHead
    {
        double bpm = 140.0, ppq = 0.0;
        bool playing = true;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo p;
            p.setBpm (bpm);
            p.setPpqPosition (ppq);
            p.setIsPlaying (playing);
            p.setTimeSignature (TimeSignature { 4, 4 });
            return p;
        }
    };
}

class TempoKeyTests final : public juce::UnitTest
{
public:
    TempoKeyTests() : juce::UnitTest ("Tempo and key set by the user", "TempoKey") {}

    void capture (ContextAudioProcessor& p, Head& head)
    {
        p.session().startCapture();
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        juce::int64 n = 0;
        const double twoPi = juce::MathConstants<double>::twoPi;
        for (int b = 0; b < 1600 && ! p.session().hasContext(); ++b)
        {
            for (int i = 0; i < 512; ++i)
            {
                const double t = (double) (n + i) / 48000.0;
                const double beat = std::fmod (t, 60.0 / head.bpm);
                const float v = (float) (std::sin (twoPi * 55.0 * beat) * std::exp (-beat * 8.0) * 0.6
                                         + 0.05 * (std::sin (twoPi * 220.0 * t) + std::sin (twoPi * 261.63 * t) + std::sin (twoPi * 329.63 * t)));
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }
            n += 512;
            p.processBlock (buf, midi);
            head.ppq += 512 * head.bpm / (60.0 * 48000.0);
            if (b % 50 == 0) juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
        }
        head.playing = false;
        waitUntil ([&] { return p.session().hasContext(); }, 20000);
    }

    void runTest() override
    {
        beginTest ("Without a captured track, your tempo is used instead of the DAW's");
        {
            ContextAudioProcessor p;
            Head head;
            head.playing = false;
            p.setPlayHead (&head);
            p.prepareToPlay (48000.0, 512);
            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            auto& s = p.session();
            expectEquals (s.sessionTempo().bpm, 140.0);
            s.setTempo (70.0);
            expectEquals (s.sessionTempo().bpm, 70.0);
            expect (s.tempoSource() == SessionController::TempoSource::Set);
            auto ui = p.getUiState();
            ui.useTrackContext = true;
            ui.type = GenerationType::Melody;
            ui.prompt = "warm keys";
            const auto out = p.previewPrompt (ui);
            expect (out.bpm.has_value() && std::abs (*out.bpm - 70.0) < 1.0e-9, "the prompt asks for your tempo");
            s.clearTempo();
            expectEquals (s.sessionTempo().bpm, 140.0, "back to the DAW's tempo");
            p.releaseResources();
        }

        beginTest ("Half-time re-reads the same captured audio; double-time goes back");
        ContextAudioProcessor p;
        Head head;
        p.setPlayHead (&head);
        p.prepareToPlay (48000.0, 512);
        capture (p, head);
        auto& s = p.session();
        expect (s.hasContext());
        if (! s.hasContext())
            return;
        const auto before = *s.getState().context;
        expectEquals (before.bars, 8);
        s.setTempo (70.0);
        auto ctx = *s.getState().context;
        expectEquals (ctx.tempo.bpm, 70.0);
        expectEquals (ctx.bars, 4, "8 bars at 140 = 4 bars at 70");
        expect (ctx.lengthSamples == before.lengthSamples && ctx.id == before.id, "same audio, nothing stretched");
        expect (! s.hostTempoDiffers(), "half-time of the DAW tempo still plays in time with it");
        expectEquals (s.contextForUpload().bars, 4, "the track is sent as 4 bars at the new tempo");
        s.setTempo (140.0);
        ctx = *s.getState().context;
        expect (ctx.bars == 8 && std::abs (ctx.tempo.bpm - 140.0) < 1.0e-9);

        beginTest ("Another tempo re-fits the captured track to it");
        s.setTempo (150.0);
        expect (waitUntil ([&] { auto c = s.getState().context; return c && std::abs (c->tempo.bpm - 150.0) < 1.0e-6; }, 30000));
        ctx = *s.getState().context;
        expectEquals (ctx.bars, 8, "still 8 bars");
        const auto expectedLen = musical::samplesForBars (8, ctx.tempo, ctx.sampleRate);
        expect (std::abs (ctx.lengthSamples - expectedLen) <= 2, "8 bars at 150 BPM long");
        expect (s.hostTempoDiffers(), "the DAW is still at 140: the header offers to fit it back");

        beginTest ("The key you pick is used in the prompt, over the detected one; Detect goes back");
        const auto detected = s.detectedKey();
        s.setKey (1, true);
        const auto k = s.sessionKey();
        expect (k.chosen && k.root == 1 && k.minor);
        expectEquals (k.display(), juce::String ("C# minor"));
        auto ui = p.getUiState();
        ui.useTrackContext = true;
        ui.type = GenerationType::Melody;
        ui.prompt = "dark bells";
        auto out = p.previewPrompt (ui);
        expectEquals (out.key, juce::String ("C#"));
        expectEquals (out.mode, juce::String ("minor"));
        ui.useTrackContext = false;
        out = p.previewPrompt (ui);
        expectEquals (out.key, juce::String ("C#"), "a chosen key is used even with Analysis off");
        ui.prompt = "dark bells in E major";
        expect (p.previewPrompt (ui).key.isEmpty(), "a key typed in the prompt wins (sent as typed)");

        beginTest ("Tempo and key are saved with the project");
        {
            juce::MemoryBlock mb;
            p.getStateInformation (mb);
            ContextAudioProcessor q;
            q.prepareToPlay (48000.0, 512);
            q.setStateInformation (mb.getData(), (int) mb.getSize());
            juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
            const auto qk = q.session().sessionKey();
            expect (qk.chosen && qk.root == 1 && qk.minor, "key restored");
            expect (q.session().getState().context && std::abs (q.session().getState().context->tempo.bpm - 150.0) < 1.0e-6, "tempo restored");
            q.releaseResources();
        }
        s.setKey (-1, false);
        expect (! s.sessionKey().chosen);
        expect (s.sessionKey().root == detected.root, "Detect automatically = the detected key again");
        p.releaseResources();
    }
};

static TempoKeyTests tempoKeyTests;
} // namespace context::test
