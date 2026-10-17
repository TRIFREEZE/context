#include "TestSupport.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Generation/SynthAudio.h"

namespace context::test
{
namespace
{
    struct FakePlayHead final : juce::AudioPlayHead
    {
        double bpm = 144.0, ppq = 0.0, sampleRate = 48000.0;
        bool playing = true, hasTempo = true;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo p;
            if (hasTempo) p.setBpm (bpm);
            p.setPpqPosition (ppq);
            p.setIsPlaying (playing);
            p.setTimeSignature (TimeSignature { 4, 4 });
            return p;
        }
        void advance (int samples) { if (playing) ppq += samples * bpm / (60.0 * sampleRate); }
    };

    /** Drums + chord, so analysis finds a key and transients. */
    float musicSample (juce::int64 i, double sr)
    {
        const double t = (double) i / sr, beat = std::fmod (t, 60.0 / 144.0);
        const double twoPi = 2.0 * juce::MathConstants<double>::pi;
        return (float) (std::sin (twoPi * 55.0 * beat) * std::exp (-beat * 20.0) * 0.6
                        + 0.08 * (std::sin (twoPi * 220.0 * t) + std::sin (twoPi * 261.63 * t) + std::sin (twoPi * 329.63 * t)));
    }

    void runHost (ContextAudioProcessor& p, FakePlayHead& head, int blocks, int blockSize = 512)
    {
        juce::AudioBuffer<float> buf (2, blockSize);
        juce::MidiBuffer midi;
        static juce::int64 counter = 0;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const float v = musicSample (counter + i, head.sampleRate);
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }
            counter += blockSize;
            p.processBlock (buf, midi);
            head.advance (blockSize);
        }
    }

    juce::File writeLoop (const juce::File& dir, const juce::String& name, double seconds, double sr = 44100.0)
    {
        juce::AudioBuffer<float> b (2, (int) (seconds * sr));
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float v = musicSample (i, sr) * 0.8f;
            b.setSample (0, i, v);
            b.setSample (1, i, -v);
        }
        auto f = dir.getChildFile (name);
        juce::String err;
        assets::writeWav (b, sr, f, 24, err);
        return f;
    }
}

class SessionTests final : public juce::UnitTest
{
public:
    SessionTests() : juce::UnitTest ("Workspace session (processor level)", "V2") {}

    void runTest() override
    {
        const double sr = 48000.0;
        auto dir = scratchDir ("session");
        juce::MemoryBlock savedState;
        juce::File importedFile;

        {
            ContextAudioProcessor p;
            FakePlayHead head;
            p.setPlayHead (&head);
            p.prepareToPlay (sr, 512);
            auto& session = p.session();

            beginTest ("Capture exactly 8 bars through processBlock, then analyse and cache");
            {
                head.ppq = 2.5;   // mid-bar: capture starts at ppq 4
                session.startCapture();
                expect (session.captureStatus().armed());
                runHost (p, head, 50);   // 1.5 beats at 144 BPM = 30000 samples (~58 blocks)
                expect (session.captureStatus().armed(), "still waiting for the bar line");
                runHost (p, head, 12);
                expect (session.captureStatus().recording(), "recording after the bar line");
                runHost (p, head, 1400);   // > 8 bars at 144 BPM (13.3 s)
                expect (waitUntil ([&] { return session.hasContext(); }, 20000), "context captured and processed");
                const auto lane = session.contextLane();
                expectEquals (lane.lengthSamples, musical::samplesForBars (8, { 144.0, {} }, sr));
                expect (session.tempoSource() == SessionController::TempoSource::Context);
                expectEquals (session.sessionTempo().bpm, 144.0);
                expect (session.getState().context.has_value() && session.getState().context->startMode == "host");
                const auto a = p.analysis().getLatest();
                expect (a.has_value() && a->hostBpm == 144.0, "analysis published for prompts");
                expect (AppPaths::contextCacheDir().getChildFile ("context-" + session.getState().context->id + ".wav").existsAsFile(), "capture cached on disk");
                expect (session.lastNotice().text.contains ("Captured 8 bars"));
            }

            beginTest ("Import a 7.54-bar loop: fitted to exactly 8 bars at the session tempo");
            {
                importedFile = writeLoop (dir, "guitar_loop.wav", musical::secondsForBars (7.54, { 144.0, {} }));
                session.importFile (importedFile);
                expect (waitUntil ([&] { return session.fitStatus().hasReport && ! session.fitStatus().rendering; }, 30000));
                const auto fit = session.fitStatus();
                expect (fit.report.modeUsed == FitMode::Stretch, toLabel (fit.report.modeUsed));
                expectEquals (session.outputLane().lengthSamples, musical::samplesForBars (8, { 144.0, {} }, sr));
                expectWithinAbsoluteError (fit.report.stretchPercent, 6.1, 0.1);
                expect (session.getState().sourceKind == SessionState::SourceKind::Imported);

                session.setLengthBars (4);
                expect (waitUntil ([&] { return session.outputLane().lengthSamples == musical::samplesForBars (4, { 144.0, {} }, sr) && ! session.fitStatus().rendering; }, 30000),
                        "changing the length re-renders the asset (not a visual crop)");
                expect (session.fitStatus().report.modeUsed == FitMode::Trim, "7.54 bars into 4 bars: trimmed");
                session.setLengthBars (8);
                session.setFitMode (FitMode::Repitch);
                expect (waitUntil ([&] { return session.fitStatus().report.modeUsed == FitMode::Repitch && ! session.fitStatus().rendering; }, 30000));
                session.setFitMode (FitMode::Auto);
                expect (waitUntil ([&] { return session.fitStatus().report.modeUsed == FitMode::Stretch && ! session.fitStatus().rendering; }, 30000));
            }

            beginTest ("Play both lanes through processBlock; lanes and modes respond");
            {
                head.playing = false;
                session.play (PreviewEngine::Mode::Both);
                juce::AudioBuffer<float> silent (2, 512);
                juce::MidiBuffer midi;
                float peak = 0.0f;
                for (int i = 0; i < 40; ++i)
                {
                    silent.clear();
                    p.processBlock (silent, midi);
                    peak = juce::jmax (peak, silent.getMagnitude (0, 512));
                }
                expect (peak > 0.05f, "audible with no DAW input: " + juce::String (peak));
                expect (session.isPlaying() && session.playheadSamples() > 0);
                p.setNonRealtime (true);
                silent.clear();
                p.processBlock (silent, midi);
                expectEquals (silent.getMagnitude (0, 512), 0.0f, "never in offline bounces");
                p.setNonRealtime (false);
                session.stop();
                for (int i = 0; i < 3; ++i) { silent.clear(); p.processBlock (silent, midi); }
                silent.clear();
                p.processBlock (silent, midi);
                expectEquals (silent.getMagnitude (0, 512), 0.0f, "silent after stop");
            }

            beginTest ("Effects change the export; the export is exact-length and reused until something changes");
            {
                const auto plain = session.exportProcessed();
                expect (plain.ok(), plain.error);
                p.effects().add ("saturation");
                p.effects().add ("reverb");
                p.effects().effect (0)->parameter ("drive")->set (30.0f);
                session.processingChanged();
                const auto processed = session.exportProcessed();
                expect (processed.ok() && processed.file != plain.file, "a different render for different processing");
                auto a = assets::loadFile (plain.file, AudioAsset::Origin::Imported);
                auto b = assets::loadFile (processed.file, AudioAsset::Origin::Imported);
                expect (a.ok() && b.ok());
                if (a.ok() && b.ok())
                {
                    expectEquals (b.asset->numSamples(), musical::samplesForBars (8, { 144.0, {} }, sr));
                    expectEquals (b.asset->sampleRate, sr);
                    double d = 0.0;
                    for (int i = 0; i < 48000; ++i) d += std::abs (a.asset->audio.getSample (0, i) - b.asset->audio.getSample (0, i));
                    expect (d > 100.0, "processing is printed into the file");
                }
                expect (session.exportProcessed().file == processed.file, "unchanged state re-uses the file");
                expect (processed.file.getFileName().contains ("8bars_144BPM"), processed.file.getFileName());

                session.setCompareOriginal (true);
                const auto dry = session.exportProcessed();
                expect (dry.file.getFileName().contains ("_dry"), "A/B 'original' exports the unprocessed fit");
                session.setCompareOriginal (false);
            }

            beginTest ("Host tempo change is flagged, not applied silently; retarget re-fits the context");
            {
                head.bpm = 150.0;
                runHost (p, head, 2);
                expect (session.hostTempoDiffers());
                expectEquals (session.sessionTempo().bpm, 144.0, "capture tempo stays in charge until the user decides");
                session.retargetContextToHostTempo();
                expect (waitUntil ([&] { return session.sessionTempo().bpm == 150.0 && ! session.captureStatus().processing; }, 30000));
                expectEquals (session.contextLane().lengthSamples, musical::samplesForBars (8, { 150.0, {} }, sr));
                expect (! session.hostTempoDiffers());
                expect (waitUntil ([&] { return session.outputLane().lengthSamples == musical::samplesForBars (8, { 150.0, {} }, sr) && ! session.fitStatus().rendering; }, 30000),
                        "output re-fitted to the new tempo");
            }

            beginTest ("Generated results and Restore generated");
            {
                GenerationRequest req;
                req.userPrompt = req.expandedPrompt = "dark plucked melody";
                req.type = GenerationType::Melody;
                req.bpm = 150.0;
                req.durationSeconds = 6.0;
                const auto rendered = synth::render (req, 7);
                GenerationResult res;
                res.generationId = "gen-session-1";
                res.prompt = req.userPrompt;
                res.type = req.type;
                res.bpm = 150.0;
                res.localFile = p.services().generatedFolder().getChildFile ("Context_150BPM_Melody_test.wav");
                res.localFile.getParentDirectory().createDirectory();
                synth::writeWav (rendered.audio, rendered.sampleRate, res.localFile);
                res.durationSeconds = rendered.audio.getNumSamples() / rendered.sampleRate;
                res.creationTimestamp = juce::Time::getCurrentTime();
                p.services().history().add (res);

                p.selectResult (res.generationId);   // History > Use
                expect (waitUntil ([&] { return session.outputSource() != nullptr && session.outputSource()->origin == AudioAsset::Origin::Generated && ! session.fitStatus().rendering; }, 30000));
                expect (session.getState().generatedId == res.generationId);
                session.importFile (importedFile);
                expect (waitUntil ([&] { return session.outputSource() != nullptr && session.outputSource()->origin == AudioAsset::Origin::Imported; }, 30000));
                expect (session.canRestoreGenerated());
                session.restoreGenerated();
                expect (waitUntil ([&] { return session.outputSource() != nullptr && session.outputSource()->origin == AudioAsset::Origin::Generated; }, 30000));
                session.importFile (importedFile);
                expect (waitUntil ([&] { return session.outputSource() != nullptr && session.outputSource()->origin == AudioAsset::Origin::Imported && ! session.fitStatus().rendering; }, 30000));

                // Unsupported files are refused with a clear message.
                auto txt = dir.getChildFile ("notes.txt");
                txt.replaceWithText ("hi");
                session.importFile (txt);
                expect (session.lastNotice().error && session.lastNotice().text.contains ("isn't a supported audio file"));
            }

            beginTest ("Session settings persist in the DAW project (small state)");
            {
                session.setLaneGainDb (PreviewEngine::outputLane, -4.5f);
                session.setLaneMute (PreviewEngine::contextLane, true);
                session.setAlgorithm (stretch::Algorithm::Texture);
                session.setPitch (2.0);
                session.setLoop (false);
                p.effects().setBypassed (1, true);
                waitUntil ([&] { return ! session.fitStatus().rendering; }, 30000);
                p.getStateInformation (savedState);
                expect (savedState.getSize() < 16384, "state stays small: " + juce::String ((int) savedState.getSize()));
            }
            p.setPlayHead (nullptr);
        }

        beginTest ("Reopen the project: settings, rack and audio come back");
        {
            ContextAudioProcessor p;
            p.prepareToPlay (sr, 512);
            p.setStateInformation (savedState.getData(), (int) savedState.getSize());
            auto& session = p.session();
            const auto s = session.getState();
            expectEquals (s.outputGainDb, -4.5f);
            expect (s.contextMute && ! s.loop);
            expect (s.fit.algorithm == stretch::Algorithm::Texture && s.fit.pitchSemitones == 2.0);
            expectEquals (p.effects().size(), 2);
            expectEquals (p.effects().typeId (0), juce::String ("saturation"));
            expectEquals (p.effects().effect (0)->parameter ("drive")->get(), 30.0f);
            expect (p.effects().isBypassed (1));
            expect (waitUntil ([&] { return session.hasContext(); }, 20000), "captured context reloaded from the cache");
            expectEquals (session.sessionTempo().bpm, 150.0);
            expect (waitUntil ([&] { return session.fitStatus().hasReport && ! session.fitStatus().rendering; }, 30000), "imported sample reloaded and fitted");
            expectEquals (session.outputLane().lengthSamples, musical::samplesForBars (8, { 150.0, {} }, sr));

            // Screenshot of the full workspace (for visual review).
            p.services().settings().update ([] (Settings& st) { st.firstRunComplete = true; });
            auto ui = p.getUiState();
            ui.prompt = "dark distorted shredded electric guitar solo";
            ui.type = GenerationType::Instrument;
            p.setUiState (ui);
            p.effects().add ("compressor");
            p.effects().add ("delay");
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            for (auto size : { juce::Point<int> (1120, 800), juce::Point<int> (980, 700), juce::Point<int> (1600, 1000) })
            {
                ed->setSize (size.x, size.y);
                session.play (PreviewEngine::Mode::Both);
                juce::AudioBuffer<float> b (2, 512);
                juce::MidiBuffer m;
                for (int i = 0; i < 300; ++i) { b.clear(); p.processBlock (b, m); }
                juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
                const auto img = ed->createComponentSnapshot (ed->getLocalBounds());
                auto out = isolatedRoot().getChildFile ("ui-workspace-" + juce::String (size.x) + ".png");
                juce::FileOutputStream os (out);
                juce::PNGImageFormat().writeImageToStream (img, os);
                logMessage ("  snapshot: " + out.getFullPathName());
            }
            session.stop();
        }

        beginTest ("Old projects, malformed state and missing files are handled gracefully");
        {
            // A state saved by the previous version (no session/chain fields).
            juce::ValueTree old ("ContextState");
            old.setProperty ("version", 1, nullptr);
            old.setProperty ("prompt", "old project", nullptr);
            old.setProperty ("type", "loop", nullptr);
            juce::MemoryBlock mb;
            juce::AudioProcessor::copyXmlToBinary (*old.createXml(), mb);
            ContextAudioProcessor p;
            p.prepareToPlay (sr, 512);
            p.setStateInformation (mb.getData(), (int) mb.getSize());
            expectEquals (p.getUiState().prompt, juce::String ("old project"));
            expectEquals (p.session().getState().lengthBars, 8);
            expectEquals (p.effects().size(), 0);

            // Malformed session/chain JSON.
            juce::ValueTree bad ("ContextState");
            bad.setProperty ("version", 2, nullptr);
            bad.setProperty ("session", "{\"bars\": 7, \"fit\": 12, \"outGain\": \"loud\", \"source\": \"imported\", \"importedPath\": \"relative/path.wav\", \"context\": {\"id\": \"../x\"}}", nullptr);
            bad.setProperty ("chain", "[[[not json", nullptr);
            juce::MemoryBlock mb2;
            juce::AudioProcessor::copyXmlToBinary (*bad.createXml(), mb2);
            p.setStateInformation (mb2.getData(), (int) mb2.getSize());
            const auto s = p.session().getState();
            expectEquals (s.lengthBars, 8, "invalid bar count falls back");
            expectEquals (s.outputGainDb, 0.0f);
            expect (s.sourceKind == SessionState::SourceKind::None, "relative import paths are rejected");
            expect (! s.context.has_value(), "unsafe context ids are rejected");

            // A project whose sample was deleted.
            SessionState missing;
            missing.sourceKind = SessionState::SourceKind::Imported;
            missing.importedPath = dir.getChildFile ("deleted.wav").getFullPathName();
            p.session().restore (missing, {});
            expect (p.session().outputLane().missing);
            expect (p.session().outputLane().message.contains ("deleted.wav"));

            // Editor opens on top of all of this without trouble.
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            expect (ed != nullptr);
        }
    }
};

static SessionTests sessionTests;
} // namespace context::test
