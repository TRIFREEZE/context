#include "TestSupport.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Session/EditModel.h"
#include "Generation/SynthAudio.h"

namespace context::test
{
namespace
{
    constexpr double sr = 48000.0;
    const musical::Tempo t120 { 120.0, {} };          // 1 beat = 0.5 s = 24000 samples
    constexpr double total = 32.0;                    // 8 bars of 4/4
    const juce::int64 totalSamples = edit::samplesForBeats (total, t120, sr);

    /** A take whose every sample says which take and where it is (easy to verify). */
    std::shared_ptr<const juce::AudioBuffer<float>> ramp (float id)
    {
        auto b = std::make_shared<juce::AudioBuffer<float>> (2, (int) totalSamples);
        for (int i = 0; i < b->getNumSamples(); ++i)
        {
            const float v = id + (float) i / (float) totalSamples * 0.1f;
            b->setSample (0, i, v);
            b->setSample (1, i, -v);
        }
        return b;
    }

    juce::int64 at (double beat) { return edit::samplesForBeats (beat, t120, sr); }
}

class EditTests final : public juce::UnitTest
{
public:
    EditTests() : juce::UnitTest ("Editing: takes, clips, undo", "V2") {}

    void runTest() override
    {
        edit::TakeAudio takes { { "t1", ramp (0.2f) }, { "t2", ramp (0.5f) } };
        const juce::String base = "t1";

        beginTest ("No edits = the base take, sample for sample; a plain split changes nothing");
        {
            edit::Edit none;
            expect (! none.isEdited());
            const auto m = edit::materialise (none, base, total);
            expect (m.clips.size() == 1 && m.clips[0].take == base && m.clips[0].length == total);
            expect (! edit::simplify (m, base, total).isEdited(), "the identity arrangement simplifies back to 'no edits'");

            const auto plain = edit::render (none, base, takes, t120, sr, totalSamples);
            expectEquals ((juce::int64) plain.getNumSamples(), totalSamples);
            expectEquals (plain.getSample (0, 1000), takes["t1"]->getSample (0, 1000));

            const auto split = edit::splitAt (none, base, total, 6.0);
            expect (split.isEdited() && split.clips.size() == 2);
            expectEquals (split.clips[1].offset, 6.0);
            const auto a = edit::render (split, base, takes, t120, sr, totalSamples);
            double maxDiff = 0.0;
            for (int i = 0; i < a.getNumSamples(); ++i)
                maxDiff = juce::jmax (maxDiff, (double) std::abs (a.getSample (0, i) - plain.getSample (0, i)));
            expect (maxDiff == 0.0, "split renders bit-identically (no crossfade on a seamless join)");
        }

        beginTest ("Delete leaves silence with short fades; everything else is untouched");
        {
            const auto e = edit::removeRange ({}, base, total, 8.0, 12.0);
            expectEquals ((int) e.clips.size(), 2);
            const auto r = edit::render (e, base, takes, t120, sr, totalSamples);
            expectEquals (r.getSample (0, (int) at (10.0)), 0.0f, "deleted range is silent");
            expectEquals (r.getSample (0, (int) at (4.0)), takes["t1"]->getSample (0, (int) at (4.0)), "before is untouched");
            expectEquals (r.getSample (0, (int) at (14.0)), takes["t1"]->getSample (0, (int) at (14.0)), "after is untouched");
            const int fade = (int) (0.0025 * sr);
            expect (std::abs (r.getSample (0, (int) at (12.0))) < 0.01f, "fade-in starts from silence (no click)");
            expectWithinAbsoluteError (r.getSample (0, (int) at (12.0) + fade + 2), takes["t1"]->getSample (0, (int) at (12.0) + fade + 2), 1.0e-6f);
            expect (std::abs (r.getSample (0, (int) at (8.0) + fade + 2)) < 1.0e-6f, "tail fades out within 2.5 ms");

            auto all = edit::removeRange ({}, base, total, 0.0, total);
            expect (all.isEdited() && all.clips.empty(), "deleting everything is an edited, silent lane");
            const auto silent = edit::render (all, base, takes, t120, sr, totalSamples);
            expectEquals (silent.getMagnitude (0, silent.getNumSamples()), 0.0f);
        }

        beginTest ("Fill from another take: the same bars of take 2, crossfaded in");
        {
            const auto e = edit::fillFromTake ({}, base, total, 8.0, 16.0, "t2");
            expectEquals ((int) e.clips.size(), 3);
            expect (e.clips[1].take == "t2" && e.clips[1].offset == 8.0, "same bars of the other take");
            const auto r = edit::render (e, base, takes, t120, sr, totalSamples);
            const int mid = (int) at (12.0);
            expectWithinAbsoluteError (r.getSample (0, mid), takes["t2"]->getSample (0, mid), 1.0e-6f);
            expectWithinAbsoluteError (r.getSample (1, mid), takes["t2"]->getSample (1, mid), 1.0e-6f);
            const int join = (int) at (16.0) + 60;                        // inside the 2.5 ms crossfade
            const float mix = r.getSample (0, join);
            expect (mix > takes["t1"]->getSample (0, join) - 0.001f && mix < takes["t2"]->getSample (0, join) + 0.001f,
                    "crossfade is a blend of both takes");
            expectWithinAbsoluteError (r.getSample (0, (int) at (20.0)), takes["t1"]->getSample (0, (int) at (20.0)), 1.0e-6f);

            // Filling a range with the base take again is a no-op arrangement.
            const auto back = edit::fillFromTake (e, base, total, 0.0, total, base);
            expect (! back.isEdited(), "filling everything from the base take = no edits");
        }

        beginTest ("Move, trim and stretch clips; clips never overlap");
        {
            auto e = edit::splitAt ({}, base, total, 8.0);
            e = edit::removeRange (e, base, total, 8.0, total);                // one clip [0, 8)
            expectEquals ((int) e.clips.size(), 1);
            e = edit::moveClip (e, base, total, 0, 16.0);                      // now [16, 24) playing beats 0-8
            expect (e.clips.size() == 1 && e.clips[0].start == 16.0 && e.clips[0].offset == 0.0);
            auto r = edit::render (e, base, takes, t120, sr, totalSamples);
            expectEquals (r.getSample (0, (int) at (4.0)), 0.0f, "moved away: the old place is silent");
            expectWithinAbsoluteError (r.getSample (0, (int) at (18.0)), takes["t1"]->getSample (0, (int) at (2.0)), 1.0e-6f,
                                       "moved audio plays later, from its own start");

            e = edit::trimClip (e, base, total, 0, true, 18.0);                // start edge right by 2 beats
            expect (e.clips[0].start == 18.0 && e.clips[0].offset == 2.0 && e.clips[0].length == 6.0, "trim keeps audio in place");
            e = edit::trimClip (e, base, total, 0, false, 40.0);               // end edge beyond the timeline
            expect (e.clips[0].end() <= total, "trim is limited to the timeline");

            auto s = edit::splitAt ({}, base, total, 4.0);                     // [0,4) [4,32)
            s = edit::stretchClip (s, base, total, 0, 6.0);                    // first clip to 6 beats
            expectEquals ((int) s.clips.size(), 2);
            expectWithinAbsoluteError (s.clips[0].stretch, 4.0 / 6.0, 1.0e-9, "4 beats of audio over 6 beats");
            expect (s.clips[1].start == 6.0 && s.clips[1].offset == 6.0, "the stretched clip overwrote what it now covers");
            const auto rs = edit::render (s, base, takes, t120, sr, totalSamples);
            expectEquals ((juce::int64) rs.getNumSamples(), totalSamples);
            for (int i = 0; i < rs.getNumSamples(); i += 997)
                expect (std::isfinite (rs.getSample (0, i)));
            for (size_t i = 0; i + 1 < s.clips.size(); ++i)
                expect (s.clips[i].end() <= s.clips[i + 1].start + 1.0e-9, "no overlaps");
        }

        beginTest ("Copy, paste, duplicate; snapping; clip at position");
        {
            const auto clip = edit::copyRange ({}, base, total, 4.0, 8.0);
            expect (clip.size() == 1 && clip[0].start == 0.0 && clip[0].offset == 4.0 && clip[0].length == 4.0);
            const auto pasted = edit::paste ({}, base, total, clip, 20.0);
            const int idx = edit::clipAt (pasted, base, total, 21.0);
            expect (idx >= 0 && pasted.clips[(size_t) idx].offset == 4.0 && pasted.clips[(size_t) idx].start == 20.0);
            expectEquals (edit::snap (5.3, 1.0), 5.0);
            expectEquals (edit::snap (5.3, 0.25), 5.25);
            expectEquals (edit::snap (5.3, 0.0), 5.3);
            expectEquals (edit::snap (6.1, 4.0), 8.0);
        }

        beginTest ("Saved arrangements: round trip, junk and unknown takes are dropped");
        {
            auto e = edit::fillFromTake ({}, base, total, 8.0, 12.0, "t2");
            e = edit::setClipGain (e, base, total, 1, -6.0f);
            const auto back = edit::Edit::fromVar (e.toVar(), { "t1", "t2" });
            expect (back == e, "round trip");
            const auto filtered = edit::Edit::fromVar (e.toVar(), { "t1" });
            for (const auto& c : filtered.clips) expect (c.take == "t1", "clips of a missing take are dropped");
            const auto junk = edit::Edit::fromVar (juce::JSON::parse (R"([{"take":"t1","start":-4,"length":2},{"take":"t1","start":"x"},{"take":"t1","start":1,"length":1e300,"offset":0},7,null])"),
                                                   { "t1" });
            for (const auto& c : junk.clips)
                expect (c.start >= 0.0 && std::isfinite (c.length));
            expect (! edit::Edit::fromVar (juce::var(), { "t1" }).isEdited(), "missing = not edited");
            expect (! edit::Take::fromVar (juce::JSON::parse (R"({"id":"t1","kind":"imported","ref":"relative.wav"})")).has_value(),
                    "relative import paths are refused");
            expect (! edit::Take::fromVar (juce::JSON::parse (R"({"id":"../x","ref":"abc"})")).has_value(), "unsafe take ids are refused");
        }

        processorLevel();
    }

    GenerationResult makeResult (ContextAudioProcessor& p, const juce::String& id, int seed)
    {
        GenerationRequest req;
        req.userPrompt = req.expandedPrompt = "glassy pluck loop";
        req.type = GenerationType::Melody;
        req.bpm = 120.0;
        req.durationSeconds = 16.0;
        const auto rendered = synth::render (req, seed);
        GenerationResult res;
        res.generationId = id;
        res.prompt = req.userPrompt;
        res.type = req.type;
        res.bpm = 120.0;
        res.localFile = p.services().generatedFolder().getChildFile ("Context_edit_" + id + ".wav");
        res.localFile.getParentDirectory().createDirectory();
        synth::writeWav (rendered.audio, rendered.sampleRate, res.localFile);
        res.durationSeconds = rendered.audio.getNumSamples() / rendered.sampleRate;
        res.creationTimestamp = juce::Time::getCurrentTime();
        p.services().history().add (res);
        return res;
    }

    void processorLevel()
    {
        ContextAudioProcessor p;
        p.prepareToPlay (sr, 512);
        auto& session = p.session();
        session.setManualTempo (120.0);
        auto settle = [&] { return waitUntil ([&] { return ! session.fitStatus().rendering && session.outputLane().hasAudio; }, 30000); };
        auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };

        beginTest ("A new generation becomes the base take; earlier takes stay for comping");
        const auto r1 = makeResult (p, "edit-take-1", 11);
        const auto r2 = makeResult (p, "edit-take-2", 22);
        session.generationArrived (r1);
        expect (settle());
        session.generationArrived (r2);
        expect (waitUntil ([&] { return session.getState().baseTake == "t2" && session.takes().size() == 2 && ! session.fitStatus().rendering; }, 30000));
        auto takes = session.takes();
        expect (takes.size() == 2 && takes[1].isBase && ! takes[0].isBase);
        expect (waitUntil ([&] { auto t = session.takes(); return t.size() == 2 && t[0].ready && t[1].ready; }, 30000), "both takes fitted");

        beginTest ("Fill a selection with a new variation: the next generation lands only there");
        session.requestFill (8.0, 16.0);
        expect (session.editView().pendingFill.has_value());
        const auto r3 = makeResult (p, "edit-take-3", 33);
        session.generationArrived (r3);
        expect (! session.editView().pendingFill.has_value());
        expect (waitUntil ([&] { return session.takes().size() == 3 && ! session.fitStatus().rendering && session.takes()[2].ready; }, 30000));
        settle();
        pump (100);
        auto view = session.editView();
        expect (view.edited && view.baseTake == "t2");
        const int idx = edit::clipAt (session.getState().arrangement, view.baseTake, view.totalBeats, 12.0);
        expect (idx >= 0 && view.clips[(size_t) idx].take == "t3", "bars 3-4 now come from take 3");
        expect (session.getState().generatedId == "edit-take-2", "the base take is still the one before");
        expect (session.lastNotice().text.contains ("Take 3 now plays bars 3" + juce::String::fromUTF8 ("\xe2\x80\x93") + "4"), session.lastNotice().text);

        beginTest ("Undo / redo walk back through fills, fit changes, effects and knob gestures");
        pump (500);                                  // let the history settle
        expect (session.canUndo());
        session.undo();
        expect (waitUntil ([&] { return ! session.getState().arrangement.isEdited(); }, 5000), "undo removes the fill");
        session.redo();
        expect (session.getState().arrangement.isEdited(), "redo brings it back");

        session.setFitMode (FitMode::Loop);
        pump (500);
        session.setFitMode (FitMode::Trim);
        pump (500);
        session.undo();
        expect (session.getState().fit.mode == FitMode::Loop, "undo fit change");
        session.undo();
        expect (session.getState().fit.mode == FitMode::Auto, "undo again");
        session.redo();
        expect (session.getState().fit.mode == FitMode::Loop);

        auto& chain = p.effects();
        chain.add ("eq");
        pump (500);
        expectEquals (chain.size(), 1);
        auto* gain = chain.effect (0)->parameter ("b3gain");
        session.beginGesture();
        for (int i = 1; i <= 20; ++i)
        {
            gain->set ((float) i * 0.5f);
            pump (40);                               // a slow drag: 0.8 s of changes
        }
        session.endGesture();
        expectEquals (gain->get(), 10.0f);
        session.undo();
        expectEquals (chain.size(), 1);
        expectEquals (chain.effect (0)->parameter ("b3gain")->get(), 0.0f, "one undo reverts the whole drag");
        session.undo();
        expectEquals (chain.size(), 0, "undo removes the effect");
        session.redo();
        session.redo();
        expectEquals (chain.size(), 1);
        expectEquals (chain.effect (0)->parameter ("b3gain")->get(), 10.0f, "redo restores the drag result");

        beginTest ("Edits and takes persist in the project and reload");
        session.editSplit (4.0);
        pump (400);
        juce::MemoryBlock mb;
        p.getStateInformation (mb);
        ContextAudioProcessor q;
        q.prepareToPlay (sr, 512);
        q.setStateInformation (mb.getData(), (int) mb.getSize());
        pump (200);
        const auto qs = q.session().getState();
        expect (qs.takes.size() == 3 && qs.baseTake == "t2" && qs.arrangement == session.getState().arrangement);
        expect (waitUntil ([&] { return q.session().outputLane().hasAudio && ! q.session().fitStatus().rendering; }, 30000));
        expect (! q.session().canUndo(), "a reopened project starts a fresh undo history");

        beginTest ("Keyboard: Space plays, S splits at the playhead, Delete clears a selection, Ctrl+Z / Ctrl+Shift+Z undo and redo");
        {
            p.services().settings().update ([] (Settings& st) { st.firstRunComplete = true; });
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            auto* editor = dynamic_cast<ContextAudioProcessorEditor*> (ed.get());
            expect (editor != nullptr);
            if (editor != nullptr)
            {
                editor->showTab ("generate");
                session.stop();
                expect (editor->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey)));
                expect (session.isPlaying(), "Space starts playback");
                editor->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
                expect (! session.isPlaying(), "Space stops it");

                pump (400);
                const auto clipsBefore = session.editView().clips.size();
                session.seek (edit::samplesForBeats (6.0, session.sessionTempo(), session.renderRate()));
                expect (editor->keyPressed (juce::KeyPress ('s')));
                pump (400);
                expectEquals ((int) session.editView().clips.size(), (int) clipsBefore + 1, "S split at the playhead");

                auto& overlay = editor->getWorkspace().getLanes().arrangement();
                overlay.setSelection (std::make_pair (8.0, 12.0));
                expect (editor->keyPressed (juce::KeyPress (juce::KeyPress::deleteKey)));
                pump (400);
                expect (edit::clipAt (session.getState().arrangement, session.editView().baseTake, session.editView().totalBeats, 10.0) < 0,
                        "Delete silenced the selection");
                expect (editor->keyPressed (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0)));
                expect (edit::clipAt (session.getState().arrangement, session.editView().baseTake, session.editView().totalBeats, 10.0) >= 0,
                        "Ctrl+Z brought it back");
                expect (editor->keyPressed (juce::KeyPress ('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)));
                expect (edit::clipAt (session.getState().arrangement, session.editView().baseTake, session.editView().totalBeats, 10.0) < 0,
                        "Ctrl+Shift+Z redid it");
                expect (editor->keyPressed (juce::KeyPress ('l')));
                expect (! session.getState().loop, "L toggles loop");
                editor->keyPressed (juce::KeyPress ('l'));
                expect (editor->keyPressed (juce::KeyPress ('3')));
                expect (session.getState().playMode == PreviewEngine::Mode::Output, "3 = hear the sound only");
                editor->keyPressed (juce::KeyPress ('1'));
            }
        }

        beginTest ("Removing a take silences only its clips; the base can't be removed");
        session.removeTake ("t2");
        expectEquals ((int) session.takes().size(), 3, "base take stays");
        session.removeTake ("t3");
        expectEquals ((int) session.takes().size(), 2);
        for (const auto& c : session.getState().arrangement.clips)
            expect (c.take != "t3");
    }
};

static EditTests editTests;
} // namespace context::test
