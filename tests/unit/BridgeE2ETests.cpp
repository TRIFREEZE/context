// Runs only with:  ContextTests --bridge-e2e=<bridge URL>
// Drives Context's real LocalGenerationBridgeProvider + GenerationController through an external
// bridge (tools/kie_bridge, started against a fake Kie.ai by tests/integration/run_kie_bridge_e2e.py).
#include "TestSupport.h"
#include "Generation/GenerationController.h"
#include "Generation/LocalGenerationBridgeProvider.h"
#include "Networking/AudioFileValidator.h"

namespace context::test
{
class BridgeE2ETests final : public juce::UnitTest
{
public:
    BridgeE2ETests() : juce::UnitTest ("Kie.ai bridge through Context's provider", "BridgeE2E") {}

    std::shared_ptr<LocalGenerationBridgeProvider> makeProvider (const juce::String& url)
    {
        LocalGenerationBridgeProvider::Options o;
        o.baseUrl = url;
        o.profile = BridgeConfiguration::contextV1();
        o.profile.pollIntervalMs = 250;
        return std::make_shared<LocalGenerationBridgeProvider> (o);
    }

    struct Outcome
    {
        GenerationController::JobSnapshot job;
        std::optional<GenerationResult> result;
    };

    Outcome generate (const juce::String& url, const juce::String& prompt, GenerationType type,
                      std::optional<double> bpm = {}, juce::String key = {}, juce::String mode = {})
    {
        auto folder = scratchDir ("kie-bridge-e2e");
        GenerationController::Dependencies d;
        d.isLicensed = [] { return true; };
        d.generatedFolder = [folder] { return folder; };
        d.tempFolder = [folder] { return folder.getChildFile ("tmp"); };
        GenerationController controller (d);
        controller.setProvider (makeProvider (url));
        controller.setGenerationTimeoutSeconds (60);

        GenerationRequest r;
        r.userPrompt = prompt;
        r.expandedPrompt = prompt;
        r.type = type;
        r.bpm = bpm;
        r.key = key;
        r.mode = mode;
        Outcome out;
        const auto e = controller.enqueue (r);
        if (! e.ok)
        {
            out.job.state = JobState::Failed;
            out.job.error = e.error;
            return out;
        }
        waitUntil ([&] { return ! controller.isBusy(); }, 90000);
        out.job = controller.getCurrentJob();
        out.result = controller.getLastResult();
        return out;
    }

    void runTest() override
    {
        const auto url = integrationBridgeUrl();
        beginTest ("Bridge URL supplied");
        expect (url.startsWith ("http://127.0.0.1:"), "run with --bridge-e2e=http://127.0.0.1:<port>");
        if (url.isEmpty())
            return;

        beginTest ("Test Connection recognises the bridge");
        {
            CancellationToken token;
            const auto t = makeProvider (url)->testConnection (token);
            expect (t.status == ConnectionStatus::Connected, toDisplayString (t.status) + " " + t.detail);
            expectEquals (t.bridgeName, juce::String ("Kie.ai Suno Bridge"));
            expect (t.capabilities.has_value() && t.capabilities->has (Feature::Cancellation));
            expect (t.capabilities.has_value() && t.capabilities->supports (GenerationType::OneShot)
                    && t.capabilities->supports (GenerationType::Loop));
        }

        beginTest ("One-shot generates a valid WAV in Context's folder");
        {
            const auto o = generate (url, "short punchy trap kick", GenerationType::OneShot, 140.0, "F#", "minor");
            expect (o.job.state == JobState::Complete, toString (o.job.state) + " " + o.job.error.technicalDetail);
            expect (o.result.has_value() && o.result->localFile.existsAsFile());
            if (o.result)
                expect (AudioFileValidator::validate (o.result->localFile).ok);
        }

        beginTest ("Loop and #song generate");
        {
            const auto loop = generate (url, "dusty boom bap drum loop", GenerationType::Loop, 92.0);
            expect (loop.job.state == JobState::Complete, loop.job.error.technicalDetail);
            const auto song = generate (url, "#song dark trap beat", GenerationType::Melody);
            expect (song.job.state == JobState::Complete, song.job.error.technicalDetail);
        }

        beginTest ("Kie.ai errors reach the producer with a useful message");
        {
            const auto credits = generate (url, "kick #credits", GenerationType::OneShot);
            expect (credits.job.state == JobState::Failed);
            expect (credits.job.error.userMessage.contains ("credits"), credits.job.error.userMessage);

            const auto filtered = generate (url, "kick #sensitive", GenerationType::OneShot);
            expect (filtered.job.state == JobState::Failed);
            expect (filtered.job.error.userMessage.containsIgnoreCase ("sensitive"), filtered.job.error.userMessage);
        }
    }
};

static BridgeE2ETests bridgeE2ETests;
} // namespace context::test
