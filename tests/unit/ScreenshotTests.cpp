// Renders the main screens in typical states to PNG files (for visual review).
// Runs only with:  ContextTests --category=Screens
#include "TestSupport.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Generation/SynthAudio.h"

namespace context::test
{
namespace
{
    struct ScreenPlayHead final : juce::AudioPlayHead
    {
        double bpm = 140.0, ppq = 0.0, sampleRate = 48000.0;
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

    /** A beat: kick on 1 and 3, snare on 2 and 4, hats, and a minor chord pad. */
    float beatSample (juce::int64 i, double sr, double bpm)
    {
        const double t = (double) i / sr, beatLen = 60.0 / bpm, inBeat = std::fmod (t, beatLen);
        const int beat = (int) (t / beatLen) % 4;
        const double twoPi = 2.0 * juce::MathConstants<double>::pi;
        double v = 0.0;
        if (beat % 2 == 0) v += std::sin (twoPi * 52.0 * inBeat * (1.0 + 2.0 * std::exp (-inBeat * 30.0))) * std::exp (-inBeat * 9.0) * 0.8;
        else v += (std::sin (twoPi * 190.0 * inBeat) * 0.3 + (std::fmod (i * 2654435761.0, 1000.0) / 500.0 - 1.0) * 0.4) * std::exp (-inBeat * 18.0);
        const double eighth = std::fmod (t, beatLen * 0.5);
        v += (std::fmod (i * 40503.0, 997.0) / 498.5 - 1.0) * 0.08 * std::exp (-eighth * 60.0);
        v += 0.06 * (std::sin (twoPi * 220.0 * t) + std::sin (twoPi * 261.63 * t) + std::sin (twoPi * 329.63 * t));
        return (float) v;
    }

    GenerationResult synthResult (ContextAudioProcessor& p, const juce::String& id, const juce::String& prompt, int seed, double seconds)
    {
        GenerationRequest req;
        req.userPrompt = req.expandedPrompt = prompt;
        req.type = GenerationType::Melody;
        req.bpm = 140.0;
        req.durationSeconds = seconds;
        const auto rendered = synth::render (req, seed);
        GenerationResult res;
        res.generationId = id;
        res.prompt = prompt;
        res.type = req.type;
        res.bpm = 140.0;
        res.localFile = p.services().generatedFolder().getChildFile ("Context_screen_" + id + ".wav");
        res.localFile.getParentDirectory().createDirectory();
        synth::writeWav (rendered.audio, rendered.sampleRate, res.localFile);
        res.durationSeconds = rendered.audio.getNumSamples() / rendered.sampleRate;
        res.creationTimestamp = juce::Time::getCurrentTime();
        p.services().history().add (res);
        return res;
    }
}

class ScreenshotTests final : public juce::UnitTest
{
public:
    ScreenshotTests() : juce::UnitTest ("UI screenshots", "Screens") {}

    void snap (juce::AudioProcessorEditor& ed, const juce::String& name)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        const auto img = ed.createComponentSnapshot (ed.getLocalBounds());
        auto out = isolatedRoot().getChildFile ("screen-" + name + ".png");
        juce::FileOutputStream os (out);
        juce::PNGImageFormat().writeImageToStream (img, os);
        logMessage ("  snapshot: " + out.getFullPathName());
    }

    void runTest() override
    {
        beginTest ("Workspace states");
        ContextAudioProcessor p;
        ScreenPlayHead head;
        p.setPlayHead (&head);
        p.prepareToPlay (48000.0, 512);
        p.services().settings().update ([] (Settings& st) { st.firstRunComplete = true; });
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        ed->setSize (1120, 800);
        snap (*ed, "empty");

        p.session().startCapture();
        snap (*ed, "armed");

        // Record 8 bars of a beat through processBlock, exactly like a DAW would.
        {
            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            juce::int64 n = 0;
            for (int b = 0; b < 1600 && ! p.session().hasContext(); ++b)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float v = beatSample (n + i, 48000.0, head.bpm);
                    buf.setSample (0, i, v);
                    buf.setSample (1, i, v * 0.9f);
                }
                n += 512;
                p.processBlock (buf, midi);
                head.ppq += 512 * head.bpm / (60.0 * 48000.0);
                if (b % 50 == 0) juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
            }
            head.playing = false;
            waitUntil ([&] { return p.session().hasContext(); }, 20000);
        }

        auto& s = p.session();
        const auto r1 = synthResult (p, "screen-1", "glassy pluck melody, dark, minor", 5, 13.7);
        const auto r2 = synthResult (p, "screen-2", "glassy pluck melody, dark, minor", 9, 13.7);
        s.generationArrived (r1);
        s.generationArrived (r2);
        waitUntil ([&] { auto t = s.takes(); return t.size() == 2 && t[0].ready && t[1].ready && ! s.fitStatus().rendering; }, 30000);
        snap (*ed, "loaded");

        s.requestFill (12.0, 20.0);
        s.generationArrived (synthResult (p, "screen-3", "glassy pluck melody, dark, minor", 13, 13.7));
        waitUntil ([&] { auto t = s.takes(); return t.size() == 3 && t[2].ready && ! s.fitStatus().rendering; }, 30000);
        s.editDelete (26.0, 28.0);
        s.editSplit (4.0);
        waitUntil ([&] { return ! s.fitStatus().rendering; }, 30000);

        for (auto type : { "saturation", "reverb", "compressor" })
            p.effects().add (type);
        s.seek (edit::samplesForBeats (9.0, s.sessionTempo(), s.renderRate()));
        if (auto* e = dynamic_cast<ContextAudioProcessorEditor*> (ed.get()))
            e->showTab ("generate");
        snap (*ed, "edited");
        if (auto* e = dynamic_cast<ContextAudioProcessorEditor*> (ed.get()))
        {
            e->getWorkspace().getLanes().arrangement().setSelection (std::make_pair (20.0, 26.0));
            snap (*ed, "selection");
            e->getWorkspace().getLanes().arrangement().setSelection (std::nullopt);
        }
        ed->setSize (980, 700);
        snap (*ed, "edited-980");
        ed->setSize (1600, 960);
        snap (*ed, "edited-1600");
        ed->setSize (1120, 800);

        for (auto type : { "eq", "delay" })
        {
            p.effects().clear();
            p.effects().add (type);
            if (juce::String (type) == "eq")
            {
                p.effects().effect (0)->parameter ("b3gain")->set (6.0f);
                p.effects().effect (0)->parameter ("b1on")->set (1.0f);
                p.effects().effect (0)->parameter ("b5gain")->set (-4.0f);
            }
            if (auto* e = dynamic_cast<ContextAudioProcessorEditor*> (ed.get()))
                e->showTab ("generate");
            snap (*ed, juce::String ("fx-") + type);
        }
        if (auto* e = dynamic_cast<ContextAudioProcessorEditor*> (ed.get()))
        {
            e->showShortcuts();
            snap (*ed, "shortcuts");
            e->closeSheet();
            e->showTab ("history");
            snap (*ed, "history");
            e->showTab ("settings");
            snap (*ed, "settings");
        }
        expect (true);
    }
};

static ScreenshotTests screenshotTests;
} // namespace context::test
