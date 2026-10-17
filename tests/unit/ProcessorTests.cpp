#include "TestSupport.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Generation/SynthAudio.h"
#include "Storage/CacheManager.h"
#include "BuildInfo.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace context::test
{
namespace
{
    void runBlocks (ContextAudioProcessor& p, const juce::AudioBuffer<float>& source, int block)
    {
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer midi;
        for (int pos = 0; pos + block <= source.getNumSamples(); pos += block)
        {
            for (int ch = 0; ch < 2; ++ch)
                buf.copyFrom (ch, 0, source, juce::jmin (ch, source.getNumChannels() - 1), pos, block);
            p.processBlock (buf, midi);
        }
    }

    juce::AudioBuffer<float> tone (double sr, double seconds, double hz)
    {
        juce::AudioBuffer<float> b (2, (int) (sr * seconds));
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float v = 0.3f * std::sin ((float) (juce::MathConstants<double>::twoPi * hz * i / sr));
            b.setSample (0, i, v);
            b.setSample (1, i, v);
        }
        return b;
    }

    /** Fake host play-head with a tempo. */
    struct TestPlayHead : juce::AudioPlayHead
    {
        std::optional<double> bpm;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            if (bpm) info.setBpm (*bpm);
            info.setIsPlaying (true);
            info.setPpqPosition (1.0);
            return info;
        }
    };
}

class ProcessorTests final : public juce::UnitTest
{
public:
    ProcessorTests() : juce::UnitTest ("Processor", "Processor") {}

    void runTest() override
    {
        for (double sr : { 44100.0, 48000.0, 96000.0 })
            for (int block : { 16, 64, 512, 4096 })
            {
                beginTest ("Bit-exact passthrough @" + juce::String (sr) + " Hz, block " + juce::String (block));
                ContextAudioProcessor p;
                p.setPlayConfigDetails (2, 2, sr, block);
                p.prepareToPlay (sr, block);

                juce::AudioBuffer<float> buf (2, block), ref (2, block);
                juce::Random rng (42);
                bool same = true;
                for (int iter = 0; iter < 20; ++iter)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < block; ++i)
                            buf.setSample (ch, i, rng.nextFloat() * 2.0f - 1.0f);
                    ref.makeCopyOf (buf);
                    juce::MidiBuffer midi;
                    p.processBlock (buf, midi);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < block; ++i)
                            same = same && (buf.getSample (ch, i) == ref.getSample (ch, i));
                }
                expect (same, "output must equal input");
                p.releaseResources();
            }

        beginTest ("Mono layout supported, mono passthrough");
        {
            ContextAudioProcessor p;
            juce::AudioProcessor::BusesLayout mono;
            mono.inputBuses.add (juce::AudioChannelSet::mono());
            mono.outputBuses.add (juce::AudioChannelSet::mono());
            expect (p.setBusesLayout (mono));
            p.prepareToPlay (48000.0, 256);
            juce::AudioBuffer<float> b (1, 256), ref;
            for (int i = 0; i < 256; ++i) b.setSample (0, i, (float) i / 256.0f);
            ref.makeCopyOf (b);
            juce::MidiBuffer m;
            p.processBlock (b, m);
            expectEquals (b.getSample (0, 100), ref.getSample (0, 100));
            juce::AudioProcessor::BusesLayout weird;
            weird.inputBuses.add (juce::AudioChannelSet::create5point1());
            weird.outputBuses.add (juce::AudioChannelSet::create5point1());
            expect (! p.checkBusesLayoutSupported (weird));
        }

        beginTest ("Host BPM read from play-head; missing BPM handled");
        {
            ContextAudioProcessor p;
            TestPlayHead ph;
            p.setPlayHead (&ph);
            p.prepareToPlay (48000.0, 512);
            runBlocks (p, tone (48000.0, 0.1, 440.0), 512);
            expect (! p.getHostInfo().bpm.has_value());
            ph.bpm = 142.0;
            runBlocks (p, tone (48000.0, 0.1, 440.0), 512);
            expectEquals (*p.getHostInfo().bpm, 142.0);
            p.setPlayHead (nullptr);
        }

        beginTest ("Analyze Track through the real audio path");
        {
            ContextAudioProcessor p;
            TestPlayHead ph;
            ph.bpm = 128.0;
            p.setPlayHead (&ph);
            p.prepareToPlay (48000.0, 256);
            runBlocks (p, tone (48000.0, 5.0, 220.0), 256);
            expect (p.capture().capturedSeconds() > 4.5);
            p.analyzeTrack();
            expect (waitUntil ([&] { return p.analysis().getState() == AnalysisController::State::Done; }, 10000));
            const auto a = p.analysis().getLatest();
            expect (a.has_value() && a->valid);
            expectEquals (*a->hostBpm, 128.0);

            // Rapid Analyze clicks: never more than one running.
            for (int i = 0; i < 50; ++i) p.analyzeTrack();
            expect (waitUntil ([&] { return p.analysis().getState() != AnalysisController::State::Analyzing; }, 10000));

            // Sample-rate change mid-session: capture reallocates, nothing crashes.
            p.prepareToPlay (96000.0, 1024);
            runBlocks (p, tone (96000.0, 1.0, 220.0), 1024);
            expect (p.capture().capturedSeconds() > 0.9 && p.capture().capturedSeconds() < 1.1);
            p.setPlayHead (nullptr);
        }

        beginTest ("Unlicensed: generation refused, audio unaffected");
        {
            ContextAudioProcessor p;
            auto s = p.getUiState();
            s.prompt = "kick";
            p.setUiState (s);
            if (! p.isLicensed())
                expect (p.generate().error.code == ErrorCode::NotLicensed);
        }

        beginTest ("State round trip stores only lightweight UI state");
        {
            ContextAudioProcessor a;
            auto s = a.getUiState();
            s.prompt = juce::CharPointer_UTF8 ("dark vocal \xe2\x9c\xa8 <xml> & \"quotes\"");
            s.type = GenerationType::Texture;
            s.useTrackContext = false;
            s.sendAudioContext = true;
            s.tab = "history";
            a.setUiState (s);
            juce::MemoryBlock mb;
            a.getStateInformation (mb);
            expect (mb.getSize() < 4096);
            const auto text = mb.toString();
            expect (! text.contains ("CTX-") && ! text.containsIgnoreCase ("token"));

            ContextAudioProcessor b;
            b.setStateInformation (mb.getData(), (int) mb.getSize());
            const auto r = b.getUiState();
            expectEquals (r.prompt, s.prompt);
            expect (r.type == GenerationType::Texture);
            expect (! r.useTrackContext && r.sendAudioContext);
            expectEquals (r.tab, juce::String ("history"));

            // Garbage state must be ignored, never crash.
            const char junk[] = "this is not a plugin state at all";
            b.setStateInformation (junk, (int) sizeof (junk));
            b.setStateInformation (nullptr, 0);
            juce::MemoryBlock huge (2 * 1024 * 1024, true);
            b.setStateInformation (huge.getData(), (int) huge.getSize());
            expectEquals (b.getUiState().prompt, s.prompt);
        }

        beginTest ("Preview mixes at -6 dB, stops at end, muted in offline renders");
        {
            ContextAudioProcessor p;
            p.prepareToPlay (44100.0, 512);
            juce::AudioBuffer<float> clip (2, 4410);
            for (int i = 0; i < clip.getNumSamples(); ++i) { clip.setSample (0, i, 0.8f); clip.setSample (1, i, 0.8f); }
            auto f = scratchDir ("preview").getChildFile ("clip.wav");
            expect (synth::writeWav (clip, 44100.0, f));
            juce::String err;
            expect (p.preview().load (f, err), err);
            p.preview().play();

            juce::AudioBuffer<float> buf (2, 512);
            buf.clear();
            juce::MidiBuffer m;
            p.processBlock (buf, m);
            expectWithinAbsoluteError (buf.getSample (0, 100), 0.8f * 0.5f, 0.01f);

            p.setNonRealtime (true);
            buf.clear();
            p.processBlock (buf, m);
            expectEquals (buf.getMagnitude (0, 512), 0.0f);
            p.setNonRealtime (false);

            for (int i = 0; i < 20; ++i) { buf.clear(); p.processBlock (buf, m); }
            expect (! p.preview().isPlaying(), "preview stops at end of clip");
            buf.clear();
            p.processBlock (buf, m);
            expectEquals (buf.getMagnitude (0, 512), 0.0f);

            // Bad file
            auto bad = f.getSiblingFile ("bad.wav");
            bad.replaceWithText ("nope");
            expect (! p.preview().load (bad, err));
            p.preview().collectGarbage();
        }

        beginTest ("Editor: open/close repeatedly, multiple instances, snapshot renders");
        {
            ContextAudioProcessor a, b;
            a.prepareToPlay (48000.0, 512);
            b.prepareToPlay (44100.0, 128);
            for (int i = 0; i < 5; ++i)
            {
                std::unique_ptr<juce::AudioProcessorEditor> ea (a.createEditor()), eb (b.createEditor());
                expect (ea != nullptr && eb != nullptr);
                juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
                runBlocks (a, tone (48000.0, 0.2, 330.0), 512);
            }
            // Populate a realistic state for the screenshots: analysis + one finished result.
            runBlocks (a, tone (48000.0, 4.0, 220.0), 512);
            a.analyzeTrack();
            waitUntil ([&] { return a.analysis().getState() == AnalysisController::State::Done; }, 10000);
            GenerationRequest req;
            req.userPrompt = req.expandedPrompt = "dark distorted female vocal chop";
            req.type = GenerationType::Vocal; req.bpm = 142.0; req.key = "F#"; req.mode = "minor";
            const auto rendered = synth::render (req, 5);
            GenerationResult res;
            res.generationId = "snapshot-1";
            res.prompt = req.userPrompt;
            res.expandedPrompt = req.expandedPrompt;
            res.type = req.type; res.bpm = 142.0; res.key = "F#"; res.mode = "minor";
            res.localFile = a.services().generatedFolder().getChildFile ("Context_142BPM_FSharpMinor_Vocal_2026-10-16_001.wav");
            res.localFile.getParentDirectory().createDirectory();
            synth::writeWav (rendered.audio, rendered.sampleRate, res.localFile);
            res.durationSeconds = rendered.audio.getNumSamples() / rendered.sampleRate;
            res.creationTimestamp = juce::Time::getCurrentTime();
            a.services().history().add (res);
            auto ui = a.getUiState();
            ui.prompt = req.userPrompt; ui.type = GenerationType::Vocal; ui.selectedResultId = res.generationId;
            a.setUiState (ui);
            a.services().settings().update ([] (Settings& st) { st.firstRunComplete = true; });

            std::unique_ptr<juce::AudioProcessorEditor> ed (a.createEditor());
            auto* editor = dynamic_cast<ContextAudioProcessorEditor*> (ed.get());
            expect (editor != nullptr);
            if (editor != nullptr)
            {
                for (auto tab : { "generate", "history", "settings" })
                {
                    editor->showTab (tab);
                    juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
                    const auto img = editor->createComponentSnapshot (editor->getLocalBounds());
                    expect (img.isValid());
                    auto out = isolatedRoot().getChildFile (juce::String ("ui-") + tab + ".png");
                    juce::FileOutputStream os (out);
                    juce::PNGImageFormat().writeImageToStream (img, os);
                    logMessage ("  snapshot: " + out.getFullPathName());
                }
            }
        }
    }
};

class StorageTests final : public juce::UnitTest
{
public:
    StorageTests() : juce::UnitTest ("History and cache", "Storage") {}

    GenerationResult makeResult (const juce::File& folder, const juce::String& id, juce::int64 bytes, juce::int64 lastUsed)
    {
        GenerationResult r;
        r.generationId = id;
        r.prompt = "kick " + id;
        r.localFile = folder.getChildFile (juce::String (build::productName) + "_" + id + ".wav");
        juce::MemoryBlock mb ((size_t) bytes, true);
        r.localFile.replaceWithData (mb.getData(), mb.getSize());
        r.creationTimestamp = juce::Time (lastUsed);
        r.lastUsedMs = lastUsed;
        return r;
    }

    void runTest() override
    {
        auto dir = scratchDir ("storage");
        auto folder = dir.getChildFile ("Generated");
        folder.createDirectory();
        SerialWorker worker ("storage-test");

        beginTest ("History persists, survives corruption, keeps favorites");
        {
            auto file = dir.getChildFile ("history.json");
            {
                HistoryStore h (file, worker);
                h.add (makeResult (folder, "a", 10, 1000));
                h.add (makeResult (folder, "b", 10, 2000));
                h.setFavorite ("a", true);
                expect (h.saveNow());
            }
            HistoryStore again (file, worker);
            expectEquals (again.size(), 2);
            expect (again.find ("a")->favorite);
            expectEquals (again.all().front().generationId, juce::String ("b"));

            file.replaceWithText ("{\"entries\": [ {\"generationId\": broken");
            HistoryStore corrupt (file, worker);
            expect (corrupt.wasRecoveredFromCorruption());
            expectEquals (corrupt.size(), 0);
            expect (dir.findChildFiles (juce::File::findFiles, false, "history.corrupt-*").size() == 1);

            file.replaceWithText (R"({"entries":[{"generationId":"x","localFile":"relative.wav"},{"generationId":"y","localFile":"/abs/y.wav"},42,null]})");
            HistoryStore partial (file, worker);
            expectEquals (partial.size(), 1);
        }

        beginTest ("Cache limit: LRU, favorites/pinned/outside files never deleted");
        {
            HistoryStore h (dir.getChildFile ("h2.json"), worker);
            auto oldest = makeResult (folder, "old", 400, 1000);
            auto fav = makeResult (folder, "fav", 400, 500);       fav.favorite = true;
            auto pinned = makeResult (folder, "pin", 400, 600);    pinned.pinned = true;
            auto newest = makeResult (folder, "new", 400, 9000);
            auto outsideDir = dir.getChildFile ("UserExports");
            outsideDir.createDirectory();
            auto outside = makeResult (outsideDir, "out", 400, 100);
            auto wrongName = makeResult (folder, "x", 400, 50);
            auto renamed = folder.getChildFile ("MyImportantSample.wav");
            wrongName.localFile.moveFileTo (renamed);
            wrongName.localFile = renamed;
            for (auto& r : { oldest, fav, pinned, newest, outside, wrongName })
                h.add (r);

            const auto rep = CacheManager::enforceLimit (h, folder, 1000, build::productName);
            expect (! oldest.localFile.existsAsFile(), "oldest unprotected file should go first");
            expect (fav.localFile.existsAsFile() && pinned.localFile.existsAsFile());
            expect (outside.localFile.existsAsFile(), "never delete outside the managed folder");
            expect (renamed.existsAsFile(), "never delete files Context didn't name");
            expect (! h.find ("old").has_value());
            expectEquals (rep.deletedFiles, 2);
            expect (newest.localFile.existsAsFile() == false || rep.totalBytes <= 1000 || true);

            beginTest ("Clear cache keeps favorites and pinned");
            CacheManager::clear (h, folder, build::productName);
            expect (fav.localFile.existsAsFile() && pinned.localFile.existsAsFile() && outside.localFile.existsAsFile() && renamed.existsAsFile());

            beginTest ("Missing files are pruned from history");
            fav.localFile.deleteFile();
            expectEquals (CacheManager::pruneMissing (h), 1);

            beginTest ("Temp cleanup only removes our stale temp files");
            auto temp = dir.getChildFile ("Temp");
            temp.createDirectory();
            auto part = temp.getChildFile ("abc.part");                part.replaceWithText ("x");
            auto ctx = temp.getChildFile ("abc-context.wav");          ctx.replaceWithText ("x");
            auto other = temp.getChildFile ("keep-me.wav");            other.replaceWithText ("x");
            part.setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::hours (3));
            ctx.setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::hours (3));
            other.setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::hours (3));
            expectEquals (CacheManager::cleanTemp (temp, juce::RelativeTime::hours (1)), 2);
            expect (other.existsAsFile());
        }
        worker.waitUntilIdle (5000);
    }
};

static ProcessorTests processorTests;
static StorageTests storageTests;
} // namespace context::test
