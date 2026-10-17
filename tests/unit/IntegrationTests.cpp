// Runs only with:  ContextTests --integration <reference bridge URL> ...
// (started automatically by tests/integration/run_bridge_integration.py via CTest)
#include "TestSupport.h"
#include "Generation/GenerationController.h"
#include "Generation/LocalGenerationBridgeProvider.h"
#include "Generation/SynthAudio.h"
#include "Networking/AudioFileValidator.h"
#include "Utils/Json.h"

namespace context::test
{
class BridgeIntegrationTests final : public juce::UnitTest
{
public:
    BridgeIntegrationTests() : juce::UnitTest ("Reference bridge end-to-end", "Integration") {}

    std::shared_ptr<LocalGenerationBridgeProvider> makeProvider (const juce::String& url, BridgeConfiguration profile = BridgeConfiguration::contextV1())
    {
        LocalGenerationBridgeProvider::Options o;
        o.baseUrl = url;
        o.profile = profile;
        o.profile.pollIntervalMs = 250;
        o.profile.requestTimeoutMs = 5000;
        return std::make_shared<LocalGenerationBridgeProvider> (o);
    }

    ConnectionStatus testStatus (const juce::String& url, BridgeConfiguration profile = BridgeConfiguration::contextV1())
    {
        CancellationToken token;
        return makeProvider (url, profile)->testConnection (token).status;
    }

    struct Rig
    {
        juce::File folder = scratchDir ("bridge-generated");
        std::unique_ptr<GenerationController> controller;
        std::shared_ptr<LocalGenerationBridgeProvider> provider;
        bool provideAudioContext = false;

        Rig (std::shared_ptr<LocalGenerationBridgeProvider> p) : provider (p)
        {
            GenerationController::Dependencies d;
            d.isLicensed = [] { return true; };
            d.generatedFolder = [this] { return folder; };
            d.tempFolder = [this] { return folder.getChildFile ("tmp"); };
            d.makeAudioContext = [this] (const juce::String& id) -> std::optional<AudioConditioningPayload>
            {
                if (! provideAudioContext) return std::nullopt;
                juce::AudioBuffer<float> b (1, 22050 * 2);
                for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (0, i, std::sin ((float) i * 0.05f) * 0.3f);
                AudioConditioningPayload payload;
                payload.wavFile = folder.getChildFile ("tmp").getChildFile (id + "-context.wav");
                payload.wavFile.getParentDirectory().createDirectory();
                synth::writeWav (b, 22050.0, payload.wavFile);
                payload.seconds = 2.0; payload.sampleRate = 22050.0;
                return payload;
            };
            controller = std::make_unique<GenerationController> (d);
            controller->setProvider (provider);
            controller->setGenerationTimeoutSeconds (60);
        }

        GenerationController::JobSnapshot run (const juce::String& prompt, int timeoutMs = 60000)
        {
            GenerationRequest r;
            r.userPrompt = prompt;
            r.expandedPrompt = prompt + " (context-enhanced)";
            r.type = GenerationType::OneShot;
            r.bpm = 140.0;
            if (provideAudioContext)
                r.audioContext = AudioConditioningPayload{};
            const auto e = controller->enqueue (r);
            if (! e.ok)
            {
                GenerationController::JobSnapshot s;
                s.state = JobState::Failed;
                s.error = e.error;
                return s;
            }
            waitUntil ([this] { return ! controller->isBusy(); }, timeoutMs);
            return controller->getCurrentJob();
        }
    };

    void runTest() override
    {
        const auto url = integrationBridgeUrl();
        beginTest ("Bridge URL supplied");
        expect (url.startsWith ("http://"), "run with --integration=http://127.0.0.1:<port>");
        if (url.isEmpty())
            return;

        beginTest ("Connection statuses");
        {
            CancellationToken token;
            auto p = makeProvider (url);
            const auto t = p->testConnection (token);
            expect (t.status == ConnectionStatus::Connected, toDisplayString (t.status) + " " + t.detail);
            expect (t.capabilities.has_value() && t.capabilities->has (Feature::AudioConditioning));
            expectEquals (t.bridgeName, juce::String ("Context Reference Bridge"));

            if (integrationUrls().containsKey ("wrong-protocol"))
                expect (testStatus (integrationUrls()["wrong-protocol"]) == ConnectionStatus::UnsupportedApi);
            if (integrationUrls().containsKey ("no-generate"))
                expect (testStatus (integrationUrls()["no-generate"]) == ConnectionStatus::GenerationEndpointMissing);
            if (integrationUrls().containsKey ("closed"))
            {
                const auto s = testStatus (integrationUrls()["closed"]);
                expect (s == ConnectionStatus::ConnectionRefused || s == ConnectionStatus::Disconnected, toDisplayString (s));
            }
            expect (testStatus ("http://192.168.1.50:3000") == ConnectionStatus::NonLocalBlocked);
            expect (testStatus ("http://bridge.example.com") == ConnectionStatus::NonLocalBlocked);
            expect (testStatus ("not a url") == ConnectionStatus::InvalidAddress);

            auto badProfile = BridgeConfiguration::contextV1();
            badProfile.healthPath = "/does/not/exist";
            expect (testStatus (url, badProfile) == ConnectionStatus::UnsupportedApi);
        }

        beginTest ("Normal generation end to end");
        {
            Rig rig (makeProvider (url));
            const auto job = rig.run ("short punchy trap kick");
            expect (job.state == JobState::Complete, toString (job.state) + " " + job.error.technicalDetail);
            const auto res = rig.controller->getLastResult();
            expect (res.has_value() && res->localFile.existsAsFile());
            if (res)
            {
                expect (AudioFileValidator::validate (res->localFile).ok);
                expect (res->remoteUrl.startsWith (url));
                expect (res->providerId == "bridge");
            }
        }

        beginTest ("Unicode and very long prompts survive the round trip");
        {
            Rig rig (makeProvider (url));
            const auto job = rig.run (juce::String (juce::CharPointer_UTF8 ("\xf0\x9f\x94\xa5 dark vocal \xe2\x80\x94 \xc3\xbc" "n" "\xc3\xaf" "c" "\xc3\xb6" "d" "\xc3\xa9 \"quotes\" \\ back\\slash"))
                                      + juce::String::repeatedString (" layered", 100));
            expect (job.state == JobState::Complete, toId (job.error.code) + " " + job.error.technicalDetail);
        }

        beginTest ("Audio context is uploaded only when requested, temp file removed");
        {
            Rig rig (makeProvider (url));
            rig.controller->testConnection();   // learn capabilities (audio_conditioning)
            waitUntil ([&] { return rig.controller->getConnectionResult().status == ConnectionStatus::Connected; }, 10000);
            rig.provideAudioContext = true;
            const auto job = rig.run ("vocal with context");
            expect (job.state == JobState::Complete, job.error.technicalDetail);
            CancellationToken token;
            HttpClient::Request req;
            req.url = url + "/context/v1/jobs/" + job.remoteJobId;
            const auto status = json::parseUntrusted (HttpClient::send (req, token).bodyText()).value;
            expect (json::getNumber (status, "audioContextBytes").value_or (0) > 1000, "bridge should have received audio context");
            expect (rig.folder.getChildFile ("tmp").findChildFiles (juce::File::findFiles, false).isEmpty(), "temp files left behind");

            rig.provideAudioContext = false;
            const auto job2 = rig.run ("vocal without context");
            req.url = url + "/context/v1/jobs/" + job2.remoteJobId;
            const auto status2 = json::parseUntrusted (HttpClient::send (req, token).bodyText()).value;
            expectEquals (json::getNumber (status2, "audioContextBytes").value_or (-1), 0.0);
        }

        beginTest ("Failure modes map to the right errors and never crash");
        {
            struct Case { const char* tag; ErrorCode expected; };
            const Case cases[] = {
                { "#fail",        ErrorCode::GenerationFailed },
                { "#authexpired", ErrorCode::UpstreamAuthExpired },
                { "#malformed",   ErrorCode::MalformedResponse },
                { "#huge",        ErrorCode::MalformedResponse },
                { "#deep",        ErrorCode::MalformedResponse },
                { "#badwav",      ErrorCode::DecodeFailed },
                { "#emptywav",    ErrorCode::DecodeFailed },
                { "#bigfile",     ErrorCode::DownloadTooLarge },
                { "#expired",     ErrorCode::DownloadExpired },
                { "#vanish",      ErrorCode::GenerationFailed },
                { "#remoteurl",   ErrorCode::UnexpectedSchema },
                { "#die",         ErrorCode::BridgeOffline },
            };
            for (auto& c : cases)
            {
                Rig rig (makeProvider (url));
                const auto job = rig.run (juce::String ("kick ") + c.tag, 120000);
                expect (job.state == JobState::Failed, juce::String (c.tag) + " -> " + toString (job.state));
                expect (job.error.code == c.expected, juce::String (c.tag) + " -> " + toId (job.error.code) + " (" + job.error.technicalDetail + ")");
                expect (job.error.userMessage.isNotEmpty());
                expect (rig.folder.findChildFiles (juce::File::findFiles, true, "*.wav").isEmpty(), juce::String (c.tag) + " left a file behind");
                expect (rig.folder.getChildFile ("tmp").findChildFiles (juce::File::findFiles, false).isEmpty(), juce::String (c.tag) + " left a temp file");
            }
        }

        beginTest ("Transient 500s are retried");
        {
            Rig rig (makeProvider (url));
            const auto job = rig.run ("kick #flaky");
            expect (job.state == JobState::Complete, toId (job.error.code) + " " + job.error.technicalDetail);
        }

        beginTest ("Cancel mid-generation reaches the bridge");
        {
            Rig rig (makeProvider (url));
            GenerationRequest r;
            r.userPrompt = r.expandedPrompt = "pad #slow";
            rig.controller->enqueue (r);
            expect (waitUntil ([&] { return rig.controller->getCurrentJob().remoteJobId.isNotEmpty(); }, 10000));
            rig.controller->cancelActive();
            expect (waitUntil ([&] { return ! rig.controller->isBusy(); }, 10000));
            expect (rig.controller->getCurrentJob().state == JobState::Cancelled);
            CancellationToken token;
            HttpClient::Request req;
            req.url = url + "/context/v1/jobs/" + rig.controller->getCurrentJob().remoteJobId;
            const auto status = json::parseUntrusted (HttpClient::send (req, token).bodyText()).value;
            expectEquals (json::getString (status, "status").value_or (""), juce::String ("cancelled"));
        }

        beginTest ("Bridge offline during generation");
        {
            if (integrationUrls().containsKey ("closed"))
            {
                Rig rig (makeProvider (integrationUrls()["closed"]));
                const auto job = rig.run ("kick", 30000);
                expect (job.state == JobState::Failed);
                expect (job.error.code == ErrorCode::BridgeOffline, toId (job.error.code));
                expectEquals (job.error.userMessage, juce::String ("Local generation bridge is offline."));
            }
        }

        beginTest ("Deleting the controller while a bridge request is in flight");
        {
            auto rig = std::make_unique<Rig> (makeProvider (url));
            GenerationRequest r;
            r.userPrompt = r.expandedPrompt = "pad #slow";
            rig->controller->enqueue (r);
            juce::Thread::sleep (300);
            const auto start = juce::Time::getMillisecondCounter();
            rig.reset();
            expect (juce::Time::getMillisecondCounter() - start < 5000);
        }
    }
};

static BridgeIntegrationTests bridgeIntegrationTests;
} // namespace context::test
