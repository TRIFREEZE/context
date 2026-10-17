// Keep / Pass: verdicts are stored, passes steer the next takes of the same prompt (prompt words +
// negative tags), the bar over a new take works from the editor, and verdicts can be exported.
#include "TestSupport.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Generation/Taste.h"
#include "Generation/PromptBuilder.h"
#include "Generation/BridgeApiAdapter.h"
#include "Generation/SynthAudio.h"
#include "Utils/UrlSafety.h"
#include "Utils/Json.h"

namespace context::test
{
namespace
{
    GenerationResult rated (const juce::String& id, const juce::String& prompt, GenerationType t, int rating, juce::StringArray why = {})
    {
        GenerationResult r;
        r.generationId = id;
        r.prompt = prompt;
        r.type = t;
        r.creationTimestamp = juce::Time::getCurrentTime();
        if (rating != 0) r.metadata.set ("rating", rating);
        juce::Array<juce::var> a;
        for (auto& w : why) a.add (w);
        if (! a.isEmpty()) r.metadata.set ("passReasons", a);
        return r;
    }

    GenerationResult synthTake (ContextAudioProcessor& p, const juce::String& id, const juce::String& prompt, int seed)
    {
        GenerationRequest req;
        req.userPrompt = req.expandedPrompt = prompt;
        req.type = GenerationType::Loop;
        req.bpm = 120.0;
        req.durationSeconds = 8.0;
        const auto rendered = synth::render (req, seed);
        GenerationResult res;
        res.generationId = id;
        res.prompt = prompt;
        res.expandedPrompt = prompt + ". Seamless, loopable phrase.";
        res.type = req.type;
        res.bpm = 120.0;
        res.creationTimestamp = juce::Time::getCurrentTime();
        res.localFile = p.services().generatedFolder().getChildFile ("Context_taste_" + id + ".wav");
        res.localFile.getParentDirectory().createDirectory();
        synth::writeWav (rendered.audio, rendered.sampleRate, res.localFile);
        res.durationSeconds = rendered.audio.getNumSamples() / rendered.sampleRate;
        res.sampleRate = rendered.sampleRate;
        res.channelCount = 2;
        p.services().history().add (res);
        return res;
    }
}

class TasteTests final : public juce::UnitTest
{
public:
    TasteTests() : juce::UnitTest ("Keep / Pass and steering", "Taste") {}

    void runTest() override
    {
        beginTest ("Passes on the same prompt steer it; other prompts and types don't");
        {
            std::vector<GenerationResult> h {                        // newest first
                rated ("a", "Dark bells", GenerationType::Loop, -1, { "busy", "timing" }),
                rated ("b", "dark bells.", GenerationType::Loop, -1, { "busy" }),
                rated ("c", "dark bells", GenerationType::Loop, 1),
                rated ("d", "dark bells", GenerationType::Melody, -1, { "quality" }),
                rated ("e", "warm pads", GenerationType::Loop, -1, { "key" }),
                rated ("f", "dark bells", GenerationType::Loop, 0),
            };
            const auto s = taste::forPrompt (h, "  DARK BELLS ", GenerationType::Loop);
            expectEquals (s.passes, 2);
            expectEquals (s.keeps, 1);
            expectEquals (s.labels.joinIntoString ("|"), juce::String ("Too busy|Timing"), "most-mentioned first");
            expect (s.hints[0].contains ("sparse") && s.hints[1].contains ("tight timing"));
            expect (s.avoid.contains ("cluttered") && s.avoid.contains ("tempo changes") && ! s.avoid.contains ("off-key"));
            expect (taste::forPrompt (h, "something else", GenerationType::Loop).isEmpty());

            // Too busy and Too empty contradict: the more frequent one wins.
            std::vector<GenerationResult> h2 { rated ("x", "pad", GenerationType::Loop, -1, { "empty" }),
                                               rated ("y", "pad", GenerationType::Loop, -1, { "busy" }),
                                               rated ("z", "pad", GenerationType::Loop, -1, { "busy" }) };
            const auto s2 = taste::forPrompt (h2, "pad", GenerationType::Loop);
            expect (s2.labels.contains ("Too busy") && ! s2.labels.contains ("Too empty"), s2.labels.joinIntoString (","));
        }

        beginTest ("The steer reaches the prompt and goes out as negative tags (also with Enhance off)");
        {
            PromptBuilder::Inputs in;
            in.userPrompt = "dark bells";
            in.type = GenerationType::Loop;
            in.steerHints = { "sparse, minimal, space between notes" };
            in.avoid = { "busy", "cluttered" };
            auto out = PromptBuilder::build (in);
            expect (out.expandedPrompt.contains ("Sparse, minimal, space between notes."), out.expandedPrompt);
            expectEquals (out.negativeTags, juce::String ("busy, cluttered"));
            in.expansionEnabled = false;
            out = PromptBuilder::build (in);
            expectEquals (out.expandedPrompt, juce::String ("dark bells"), "sent as typed");
            expectEquals (out.negativeTags, juce::String ("busy, cluttered"), "but the things to avoid still go");

            GenerationRequest r;
            r.userPrompt = r.expandedPrompt = "dark bells";
            r.negativeTags = "busy, cluttered";
            const auto rt = GenerationRequest::fromVar (r.toVar());
            expectEquals (rt.negativeTags, juce::String ("busy, cluttered"), "kept in history");
            BridgeApiAdapter a (BridgeConfiguration::contextV1(), urls::parseBaseUrl ("http://127.0.0.1:3000"));
            const auto body = json::parseUntrusted (a.serializeGenerateRequest (r, nullptr)).value;
            expectEquals (json::getString (body, "negativeTags").value_or (""), juce::String ("busy, cluttered"), "sent to the bridge");
        }

        beginTest ("Export: verdicts only, as JSON, no audio");
        {
            std::vector<GenerationResult> h { rated ("a", "Dark bells", GenerationType::Loop, -1, { "busy" }),
                                              rated ("b", "warm pad", GenerationType::Texture, 1),
                                              rated ("c", "kick", GenerationType::OneShot, 0) };
            const auto parsed = juce::JSON::parse (taste::exportRatings (h));
            const auto* rows = parsed.getProperty ("ratings", {}).getArray();
            expect (rows != nullptr && rows->size() == 2, "unrated entries are left out");
            if (rows != nullptr && rows->size() == 2)
            {
                expectEquals (rows->getReference (0).getProperty ("verdict", {}).toString(), juce::String ("passed"));
                expectEquals (rows->getReference (0).getProperty ("reasons", {})[0].toString(), juce::String ("busy"));
                expectEquals (rows->getReference (1).getProperty ("verdict", {}).toString(), juce::String ("kept"));
            }
            expectEquals (parsed.getProperty ("format", {}).toString(), juce::String ("context-ratings-v1"));
        }

        beginTest ("Editor: the bar asks about a new take; Keep keeps it, Pass removes it and steers the prompt");
        {
            auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
            ContextAudioProcessor p;
            p.prepareToPlay (48000.0, 512);
            p.services().settings().update ([] (Settings& s) { s.firstRunComplete = true; s.promptExpansion = true; });
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            auto* editor = dynamic_cast<ContextAudioProcessorEditor*> (ed.get());
            expect (editor != nullptr);
            if (editor == nullptr)
                return;
            editor->showTab ("generate");
            auto& s = p.session();
            auto& bar = editor->getWorkspace().getLanes().verdictBar();
            const juce::String prompt = "taste test glassy bells " + juce::String (juce::Random::getSystemRandom().nextInt (100000));

            const auto r1 = synthTake (p, "taste-1", prompt, 3);
            s.generationArrived (r1);
            expect (waitUntil ([&] { pump (20); return bar.isVisible() && bar.isAsking() && bar.generationId() == "taste-1"; }, 30000),
                    "the bar asks about the new take");
            bar.keepButton().triggerClick();
            pump (300);
            auto h1 = p.services().history().find ("taste-1");
            expect (h1 && taste::ratingOf (*h1) == 1 && h1->favorite, "kept = rating +1 and a favorite");
            expect (! bar.isAsking(), "the bar shows the confirmation");

            const auto r2 = synthTake (p, "taste-2", prompt, 7);
            s.generationArrived (r2);
            expect (waitUntil ([&] { pump (20); return bar.isAsking() && bar.generationId() == "taste-2"; }, 30000),
                    "asks again for the next new take");
            expectEquals ((int) s.takes().size(), 2);
            bar.passButton().triggerClick();
            pump (200);
            if (auto* busy = bar.reasonButton ("busy")) busy->triggerClick();
            pump (100);
            bar.confirmPassButton().triggerClick();
            pump (500);
            auto h2 = p.services().history().find ("taste-2");
            expect (h2 && taste::ratingOf (*h2) == -1, "passed = rating -1");
            expect (h2 && h2->metadata.getWithDefault ("passReasons", {})[0].toString() == "busy", "with the reason");
            expectEquals ((int) s.takes().size(), 1, "the passed take left the lane");
            expectEquals (s.getState().baseTake, juce::String ("t1"), "the kept take plays again");

            auto ui = p.getUiState();
            ui.prompt = prompt;
            ui.type = GenerationType::Loop;
            const auto out = p.previewPrompt (ui);
            expect (out.expandedPrompt.contains ("Sparse, minimal"), "the next prompt is steered: " + out.expandedPrompt);
            expect (out.negativeTags.contains ("cluttered"), out.negativeTags);

            ed.reset();
            p.releaseResources();
        }
    }
};

static TasteTests tasteTests;
} // namespace context::test
