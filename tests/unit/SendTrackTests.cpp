// "Send my track" from the user's side: the button, the consent sheet, and what actually goes
// out with the request. Uses a stub provider that accepts audio context, so no network is needed.
#include "TestSupport.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Generation/IAudioGenerationProvider.h"
#include "UI/Motion.h"
#include "Generation/GenerationController.h"

namespace context::test
{
namespace
{
    /** A provider that accepts track context (like the Kie.ai bridge 1.1+) and records what it got. */
    class ContextStubProvider final : public IAudioGenerationProvider
    {
    public:
        explicit ContextStubProvider (bool acceptsAudio, bool staleUntilTested = false)
            : audio (acceptsAudio), stale (staleUntilTested) {}
        juce::String id() const override { return "bridge"; }
        juce::String displayName() const override { return "Stub bridge"; }
        Capabilities caps() const
        {
            auto c = Capabilities::allTypes();
            c.features = { Feature::Cancellation };
            if (audio && ! (stale && tests.load() == 0))
            {
                c.features.insert (Feature::AudioConditioning);
                c.features.insert (Feature::Cover);
                c.features.insert (Feature::Continuation);
            }
            c.maxDurationSeconds = 60;
            return c;
        }
        ConnectionTestResult testConnection (CancellationToken&) override
        {
            ++tests;
            ConnectionTestResult r;
            r.status = ConnectionStatus::Connected;
            r.capabilities = caps();
            return r;
        }
        Capabilities getCapabilities() const override { return caps(); }
        SubmitResult submit (const GenerationRequest& r, CancellationToken&) override
        {
            {
                const juce::ScopedLock sl (lock);
                last = r;
                sentAudioBytes = r.audioContext.has_value() && r.audioContext->wavFile.existsAsFile() ? r.audioContext->wavFile.getSize() : 0;
                ++submits;
            }
            juce::Thread::sleep (200);   // long enough for the status line to be seen
            SubmitResult s;
            s.error = ProviderError::make (ErrorCode::GenerationFailed, "stub: stops after submit");
            return s;
        }
        PollResult poll (const juce::String&, CancellationToken&) override { return {}; }
        bool cancel (const juce::String&, CancellationToken&) override { return false; }
        DownloadResult download (const PollResult&, const juce::File&, CancellationToken&) override { return {}; }

        bool audio, stale;
        std::atomic<int> tests { 0 }, submits { 0 };
        juce::CriticalSection lock;
        GenerationRequest last;
        juce::int64 sentAudioBytes = 0;
    };

    template <typename T>
    T* findChild (juce::Component& root, std::function<bool (T&)> pred)
    {
        for (auto* c : root.getChildren())
        {
            if (auto* t = dynamic_cast<T*> (c))
                if (pred (*t))
                    return t;
            if (auto* found = findChild<T> (*c, pred))
                return found;
        }
        return nullptr;
    }

    bool visibleInTree (juce::Component& c)
    {
        for (auto* x = &c; x != nullptr; x = x->getParentComponent())
            if (! x->isVisible())
                return false;
        return true;
    }

    juce::TextButton* buttonNamed (juce::Component& root, const juce::String& text)
    {
        return findChild<juce::TextButton> (root, [&] (juce::TextButton& b) { return b.getButtonText() == text && visibleInTree (b); });
    }
}

class SendTrackTests final : public juce::UnitTest
{
public:
    SendTrackTests() : juce::UnitTest ("Send my track: button, consent, request", "SendTrack") {}

    struct Outcome { GenerationController::JobSnapshot job; juce::StringArray statuses; };

    /** Runs one request with "Send my track" on through a real GenerationController. */
    Outcome runJob (std::shared_ptr<ContextStubProvider> stub, bool haveAudio, juce::String mode = contextmode::layer)
    {
        auto folder = scratchDir ("send-track");
        folder.getChildFile ("tmp").createDirectory();
        GenerationController::Dependencies d;
        d.isLicensed = [] { return true; };
        d.generatedFolder = [folder] { return folder; };
        d.tempFolder = [folder] { return folder.getChildFile ("tmp"); };
        d.makeAudioContext = [folder, haveAudio] (const juce::String& id) -> std::optional<AudioConditioningPayload>
        {
            if (! haveAudio)
                return std::nullopt;
            AudioConditioningPayload a;
            a.wavFile = folder.getChildFile ("tmp").getChildFile (id + "-context.wav");
            a.wavFile.replaceWithData (juce::String::repeatedString ("x", 4096).toRawUTF8(), 4096);
            a.seconds = 16.0;
            a.bars = 8;
            return a;
        };
        GenerationController controller (d);
        controller.setProvider (stub);
        GenerationRequest r;
        r.userPrompt = r.expandedPrompt = "sung hook";
        r.type = GenerationType::Vocal;
        r.audioContext = AudioConditioningPayload{};
        r.contextMode = mode;
        Outcome out;
        const auto e = controller.enqueue (r);
        expect (e.ok, e.error.technicalDetail);
        waitUntil ([&]
        {
            const auto t = controller.getCurrentJob().statusText;
            if (t.isNotEmpty() && ! out.statuses.contains (t))
                out.statuses.add (t);
            return ! controller.isBusy();
        }, 20000);
        out.job = controller.getCurrentJob();
        return out;
    }

    void runTest() override
    {
        beginTest ("Stale capabilities: the bridge is asked again and the track goes out");
        {
            auto stub = std::make_shared<ContextStubProvider> (true, true);
            const auto o = runJob (stub, true);
            expectEquals (stub->submits.load(), 1);
            expect (stub->tests.load() >= 1, "re-tested the bridge before deciding");
            expect (stub->sentAudioBytes == 4096, "the track file was there at submit");
            expect (stub->last.audioContext.has_value() && stub->last.contextMode == contextmode::layer);
        }

        beginTest ("Happy path says what is being sent");
        {
            auto stub = std::make_shared<ContextStubProvider> (true);
            const auto o = runJob (stub, true, contextmode::cover);
            expect (stub->sentAudioBytes == 4096);
            expectEquals (stub->last.contextMode, contextmode::cover);
            expect (o.statuses.joinIntoString ("|").contains ("Sending your 8 bars (Restyle)"), o.statuses.joinIntoString ("|"));
        }

        beginTest ("A bridge without track audio: the job stops and says so (nothing is generated without the track)");
        {
            auto stub = std::make_shared<ContextStubProvider> (false);
            const auto o = runJob (stub, true);
            expectEquals (stub->submits.load(), 0, "never submitted");
            expect (o.job.state == JobState::Failed);
            expect (o.job.error.userMessage.contains ("Your track wasn't sent") && o.job.error.userMessage.contains ("1.1.0"), o.job.error.userMessage);
        }

        beginTest ("Nothing heard yet: the job stops and says how to fix it");
        {
            auto stub = std::make_shared<ContextStubProvider> (true);
            const auto o = runJob (stub, false);
            expectEquals (stub->submits.load(), 0, "never submitted");
            expect (o.job.state == JobState::Failed);
            expect (o.job.error.userMessage.contains ("hasn't heard any audio"), o.job.error.userMessage);
        }

        runFlow (false);
        ui::motion::setAllowed (true);
        ui::motion::setEnabled (true);
        runFlow (true);
        ui::motion::setAllowed (false);
    }

    void runFlow (bool animated)
    {
        const juce::String tag = animated ? " [animated]" : "";
        auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
        beginTest (juce::String ("Editor opens with a bridge that accepts track audio") + tag);
        ContextAudioProcessor p;
        p.prepareToPlay (44100.0, 512);
        p.services().settings().update ([] (Settings& s)
        {
            s.firstRunComplete = true;
            s.allowSendAudioContext = false;
        });
        auto ui = p.getUiState();
        ui.sendAudioContext = false;
        p.setUiState (ui);

        auto stub = std::make_shared<ContextStubProvider> (true);
        p.generation().setProvider (stub);
        pump (100);

        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        auto* editor = dynamic_cast<ContextAudioProcessorEditor*> (ed.get());
        expect (editor != nullptr);
        if (editor == nullptr)
            return;
        editor->setVisible (true);
        editor->showTab ("generate");
        pump (300);

        beginTest (juce::String ("The button is clickable when the bridge accepts track audio") + tag);
        auto* send = buttonNamed (*editor, "Send my track");
        expect (send != nullptr, "Send my track button found");
        if (send == nullptr)
            return;
        expect (send->isEnabled(), "enabled for a bridge with audio_conditioning");
        expect (! send->getToggleState());

        beginTest (juce::String ("First click asks for consent; 'Allow and turn on' turns it on and keeps it on") + tag);
        send->triggerClick();
        pump (300);
        expect (editor->hasSheet(), "consent sheet is shown");
        auto* allow = buttonNamed (*editor, "Allow and turn on");
        expect (allow != nullptr, "consent button found");
        if (allow != nullptr)
            allow->triggerClick();
        expect (waitUntil ([&] { return ! editor->hasSheet(); }, 3000), "sheet closes");
        pump (500);
        expect (p.services().settings().get().allowSendAudioContext, "consent saved");
        expect (p.getUiState().sendAudioContext, "UI state says send");
        expect (send->getToggleState(), "the button shows ON after consent");

        beginTest (juce::String ("Clicking it off and on again works without asking again") + tag);
        send->triggerClick();
        pump (300);
        expect (! send->getToggleState() && ! p.getUiState().sendAudioContext, "turned off");
        send->triggerClick();
        pump (300);
        expect (! editor->hasSheet(), "no second consent sheet");
        expect (send->getToggleState() && p.getUiState().sendAudioContext, "turned on again");

        ed.reset();
        p.releaseResources();
    }
};

static SendTrackTests sendTrackTests;
} // namespace context::test
