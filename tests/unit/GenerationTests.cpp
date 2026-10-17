#include "TestSupport.h"
#include "Generation/GenerationController.h"
#include "Generation/MockAudioGenerationProvider.h"
#include "Generation/BridgeApiAdapter.h"
#include "Generation/SynthAudio.h"
#include "Networking/AudioFileValidator.h"
#include "Utils/Json.h"

namespace context::test
{
namespace
{
    HttpClient::Response ok (const juce::String& body, int status = 200)
    {
        HttpClient::Response r;
        r.connected = true;
        r.statusCode = status;
        r.body.append (body.toRawUTF8(), body.getNumBytesAsUTF8());
        return r;
    }

    BridgeApiAdapter v1Adapter()
    {
        return BridgeApiAdapter (BridgeConfiguration::contextV1(), urls::parseBaseUrl ("http://127.0.0.1:3000"));
    }

    GenerationRequest basicRequest (const juce::String& prompt, GenerationType t = GenerationType::OneShot)
    {
        GenerationRequest r;
        r.userPrompt = prompt;
        r.expandedPrompt = prompt;
        r.type = t;
        r.seed = 1234;
        return r;
    }
}

class ModelTests final : public juce::UnitTest
{
public:
    ModelTests() : juce::UnitTest ("Generation models", "Generation") {}

    void runTest() override
    {
        beginTest ("Request serialisation round trip");
        GenerationRequest r = basicRequest (juce::CharPointer_UTF8 ("dark vocal \xe2\x9c\xa8 \"quoted\" \\ chop"), GenerationType::Vocal);
        r.bpm = 142.0; r.key = "F#"; r.mode = "minor"; r.keyConfidence = 0.74; r.instrumental = false; r.durationSeconds = 4.0;
        r.requestId = "abc";
        const auto text = json::toCompactString (r.toVar());
        const auto back = GenerationRequest::fromVar (json::parseUntrusted (text).value);
        expectEquals (back.userPrompt, r.userPrompt);
        expect (back.type == GenerationType::Vocal);
        expectEquals (*back.bpm, 142.0);
        expectEquals (back.key, juce::String ("F#"));
        expectEquals (*back.seed, (juce::int64) 1234);
        expectEquals (*back.durationSeconds, 4.0);

        beginTest ("Request validation");
        expect (basicRequest ("").validate().isNotEmpty());
        expect (basicRequest ("   ").validate().isNotEmpty());
        expect (basicRequest (juce::String::repeatedString ("x", 1001)).validate().isNotEmpty());
        auto bad = basicRequest ("kick"); bad.bpm = 5000.0;
        expect (bad.validate().isNotEmpty());
        expect (basicRequest ("kick").validate().isEmpty());

        beginTest ("Result from corrupt var is rejected");
        expect (! GenerationResult::fromVar (juce::var()).has_value());
        expect (! GenerationResult::fromVar (json::parseUntrusted (R"({"generationId":"x","localFile":"relative/path.wav"})").value).has_value());
    }
};

class BridgeProfileTests final : public juce::UnitTest
{
public:
    BridgeProfileTests() : juce::UnitTest ("Bridge configuration", "Generation") {}

    void runTest() override
    {
        beginTest ("Built-in profile is valid");
        expectEquals (BridgeConfiguration::contextV1().validate(), juce::String());

        beginTest ("Field-map profile parses");
        const auto good = json::parseUntrusted (R"({
            "id": "my-bridge", "name": "My bridge", "requestFormat": "fieldmap",
            "healthPath": "/health", "generatePath": "/api/generate", "statusPath": "/api/jobs/{id}", "cancelPath": "",
            "supportsCancellation": false,
            "requestFields": { "prompt": "text", "bpm": "tempo" },
            "staticFields": { "model": "v1", "count": 1 },
            "response": { "jobIdPath": "data.0.id", "statusPath": "data.0.state", "audioUrlPath": "data.0.url",
                          "statusMap": { "done": "complete", "running": "generating", "err": "failed" } }
        })").value;
        BridgeConfiguration c;
        expectEquals (BridgeConfiguration::fromVar (good, c), juce::String());
        expectEquals (c.jobIdField, juce::String ("data.0.id"));
        expect (c.statusMap.at ("done") == JobState::Complete);

        beginTest ("Dangerous or invalid profiles rejected");
        auto expectRejected = [this] (const char* jsonText, const char* why)
        {
            BridgeConfiguration out;
            expect (BridgeConfiguration::fromVar (json::parseUntrusted (jsonText).value, out).isNotEmpty(), why);
        };
        expectRejected (R"({"id":"x","generatePath":"http://evil.com/gen","requestFields":{"prompt":"p"}})", "absolute URL path");
        expectRejected (R"({"id":"x","generatePath":"/../../etc","requestFields":{"prompt":"p"}})", "traversal");
        expectRejected (R"({"id":"x","statusPath":"/jobs","requestFields":{"prompt":"p"}})", "status without {id}");
        expectRejected (R"({"id":"x","requestFields":{"prompt":"p","cookie":"c"}})", "unknown source field");
        expectRejected (R"({"id":"x","requestFields":{"bpm":"b"}})", "no prompt mapping");
        expectRejected (R"({"id":"x","requestFields":{"prompt":"p"},"staticFields":{"o":{"nested":1}}})", "nested static field");
        expectRejected (R"({"id":"x","requestFields":{"prompt":"p"},"response":{"statusMap":{"done":"rm -rf"}}})", "bad state");
        expectRejected (R"({"id":"Bad Id!","requestFields":{"prompt":"p"}})", "bad id");
        expectRejected (R"({"id":"x","requestFields":{"prompt":"p"},"pollIntervalMs":1})", "poll too fast");
        expectRejected (R"({"id":"x","requestFormat":"javascript","requestFields":{"prompt":"p"}})", "unknown format");
        expectRejected (R"({"id":"x","healthPath":"/h/{cmd}","requestFields":{"prompt":"p"}})", "unknown placeholder");

        beginTest ("Profile file: corrupt file and duplicates handled");
        auto dir = scratchDir ("profiles");
        auto f = dir.getChildFile ("bridge_profiles.json");
        f.replaceWithText ("{ not json");
        auto set = BridgeProfileSet::load (f);
        expectEquals ((int) set.profiles.size(), 1);
        expect (set.problems.size() == 1);
        f.replaceWithText (R"({"profiles":[{"id":"a","requestFields":{"prompt":"p"}},{"id":"a","requestFields":{"prompt":"p"}},{"id":"context-bridge-v1","requestFields":{"prompt":"p"}}]})");
        set = BridgeProfileSet::load (f);
        expectEquals ((int) set.profiles.size(), 2);
        expectEquals (set.problems.size(), 2);
    }
};

class BridgeAdapterTests final : public juce::UnitTest
{
public:
    BridgeAdapterTests() : juce::UnitTest ("Bridge API adapter", "Generation") {}

    void runTest() override
    {
        auto a = v1Adapter();

        beginTest ("Health");
        expect (a.parseHealth (ok (R"({"protocol":"context-bridge-v1","name":"Ref"})")).status == ConnectionStatus::Connected);
        expect (a.parseHealth (ok (R"({"protocol":"other"})")).status == ConnectionStatus::UnsupportedApi);
        expect (a.parseHealth (ok ("<html>")).status == ConnectionStatus::UnexpectedResponse);
        expect (a.parseHealth (ok ("nope", 404)).status == ConnectionStatus::UnsupportedApi);
        expect (a.parseHealth (ok ("x", 500)).status == ConnectionStatus::UnexpectedResponse);
        HttpClient::Response refused; refused.elapsedMs = 5;
        expect (a.parseHealth (refused).status == ConnectionStatus::ConnectionRefused);
        HttpClient::Response timedOut; timedOut.elapsedMs = 6000;
        expect (a.parseHealth (timedOut).status == ConnectionStatus::Disconnected);

        beginTest ("Submit parsing");
        expectEquals (a.parseSubmit (ok (R"({"jobId":"job-1"})")).jobId, juce::String ("job-1"));
        expect (a.parseSubmit (ok ("{broken")).error.code == ErrorCode::MalformedResponse, "broken: " + toId (a.parseSubmit (ok ("{broken")).error.code));
        expect (a.parseSubmit (ok ("[1,2,3]")).error.code == ErrorCode::MalformedResponse, "array: " + toId (a.parseSubmit (ok ("[1,2,3]")).error.code));
        expect (a.parseSubmit (ok (R"({"id":"wrong-field"})")).error.code == ErrorCode::UnexpectedSchema);
        expect (a.parseSubmit (ok (R"({"jobId":"../../etc/passwd"})")).error.code == ErrorCode::UnexpectedSchema);
        expect (a.parseSubmit (ok (R"({"jobId":")" + juce::String::repeatedString ("a", 500) + "\"}")).error.code == ErrorCode::UnexpectedSchema);
        expect (a.parseSubmit (ok (R"({"error":"bad"})", 401)).error.code == ErrorCode::UpstreamAuthExpired);
        expect (a.parseSubmit (ok (R"({"error":"x","errorCode":"auth_expired"})", 500)).error.code == ErrorCode::UpstreamAuthExpired);
        expect (a.parseSubmit (ok ("", 404)).error.code == ErrorCode::EndpointMissing);
        expect (a.parseSubmit (ok ("", 429)).error.code == ErrorCode::RateLimited);
        const auto e500 = a.parseSubmit (ok ("boom", 500)).error;
        expect (e500.code == ErrorCode::HttpError && e500.retryable);
        HttpClient::Response truncated = ok ("x"); truncated.truncated = true;
        expect (a.parseSubmit (truncated).error.code == ErrorCode::MalformedResponse);
        expect (a.parseSubmit (HttpClient::Response{}).error.code == ErrorCode::BridgeOffline);

        beginTest ("Poll parsing");
        auto p = a.parsePoll (ok (R"({"status":"generating","progress":45})"));
        expect (p.ok && p.state == JobState::Generating);
        expectWithinAbsoluteError (p.progress, 0.45, 1e-9);
        p = a.parsePoll (ok (R"({"status":"queued"})"));
        expect (p.ok && p.state == JobState::Queued);
        p = a.parsePoll (ok (R"({"status":"complete","audioUrl":"/files/x.wav"})"));
        expect (p.ok && p.state == JobState::Complete);
        expectEquals (p.audioLocation, juce::String ("http://127.0.0.1:3000/files/x.wav"));
        p = a.parsePoll (ok (R"({"status":"complete"})"));
        expect (! p.ok && p.error.code == ErrorCode::UnexpectedSchema);
        p = a.parsePoll (ok (R"({"status":"complete","audioUrl":"http://evil.example.com/x.wav"})"));
        expect (! p.ok);
        p = a.parsePoll (ok (R"({"status":"complete","audioUrl":"file:///etc/passwd"})"));
        expect (! p.ok);
        p = a.parsePoll (ok (R"({"status":"failed","error":"content policy","errorCode":"rejected"})"));
        expect (p.ok && p.state == JobState::Failed && p.error.code == ErrorCode::GenerationFailed);
        expect (p.error.userMessage.contains ("content policy"));
        p = a.parsePoll (ok (R"({"status":"failed","errorCode":"session_expired"})"));
        expect (p.error.code == ErrorCode::UpstreamAuthExpired);
        p = a.parsePoll (ok (R"({"status":"some-new-state"})"));
        expect (p.ok && p.state == JobState::Generating);
        p = a.parsePoll (ok (R"({"nostatus":true})"));
        expect (! p.ok && p.error.code == ErrorCode::UnexpectedSchema);
        p = a.parsePoll (ok ("gone", 404));
        expect (! p.ok && p.error.code == ErrorCode::GenerationFailed);
        p = a.parsePoll (ok (juce::String::repeatedString ("[", 5000)));
        expect (! p.ok && p.error.code == ErrorCode::MalformedResponse);

        beginTest ("Context v1 request body");
        auto req = basicRequest ("kick");
        req.expandedPrompt = "kick, punchy";
        req.bpm = 140.0;
        const auto body = json::parseUntrusted (a.serializeGenerateRequest (req, nullptr)).value;
        expectEquals (*json::getString (body, "protocol"), juce::String ("context-bridge-v1"));
        expectEquals (*json::getString (body, "prompt"), juce::String ("kick, punchy"));
        expectEquals (*json::getString (body, "userPrompt"), juce::String ("kick"));
        expectEquals (*json::getNumber (body, "bpm"), 140.0);
        expect (! json::getPath (body, "audioContext").isObject());

        beginTest ("Field-map request body only contains mapped + static fields");
        BridgeConfiguration fm;
        BridgeConfiguration::fromVar (json::parseUntrusted (R"({"id":"fm","requestFields":{"prompt":"text","bpm":"tempo","key":"k"},"staticFields":{"model":"v4"}})").value, fm);
        BridgeApiAdapter fa (fm, urls::parseBaseUrl ("http://127.0.0.1:3000"));
        const auto fb = json::parseUntrusted (fa.serializeGenerateRequest (req, nullptr)).value;
        expectEquals (*json::getString (fb, "text"), juce::String ("kick, punchy"));
        expectEquals (*json::getNumber (fb, "tempo"), 140.0);
        expect (! json::getString (fb, "k").has_value(), "absent key must be omitted");
        expectEquals (*json::getString (fb, "model"), juce::String ("v4"));
        expect (! json::getString (fb, "protocol").has_value());
    }
};

class SynthTests final : public juce::UnitTest
{
public:
    SynthTests() : juce::UnitTest ("Mock synth audio", "Generation") {}

    void runTest() override
    {
        beginTest ("Every recipe renders finite, non-silent, normalised audio");
        const char* prompts[] = { "short punchy trap kick", "snare", "open hat", "clap", "808 bass", "hard Memphis cowbell",
                                  "dark distorted female vocal chop", "ethereal reverse guitar", "wide ambient synth texture",
                                  "140 BPM percussion loop", "riser", "impact boom", "lead melody", "" };
        for (auto type : allGenerationTypes)
            for (auto* prompt : prompts)
            {
                auto r = basicRequest (juce::String (prompt).isEmpty() ? "sound" : prompt, type);
                r.bpm = 140.0; r.key = "F#"; r.mode = "minor";
                const auto out = synth::render (r, 99);
                bool finite = true;
                for (int ch = 0; ch < out.audio.getNumChannels(); ++ch)
                    for (int i = 0; i < out.audio.getNumSamples(); ++i)
                        finite = finite && std::isfinite (out.audio.getSample (ch, i));
                const float peak = out.audio.getMagnitude (0, out.audio.getNumSamples());
                expect (finite, prompt);
                expect (peak > 0.5f && peak <= 1.0f, juce::String (prompt) + " peak " + juce::String (peak));
                expect (out.audio.getNumSamples() > 1000 && out.audio.getNumSamples() < 44100 * 31);
            }

        beginTest ("Deterministic for a seed; WAV round trip validates");
        const auto a = synth::render (basicRequest ("kick"), 7), b = synth::render (basicRequest ("kick"), 7);
        expect (a.audio.getNumSamples() == b.audio.getNumSamples());
        auto f = scratchDir ("synth").getChildFile ("kick.wav");
        expect (synth::writeWav (a.audio, a.sampleRate, f));
        const auto info = AudioFileValidator::validate (f);
        expect (info.ok, info.error);
        expectEquals (info.channels, 2);

        beginTest ("Validator rejects bad files");
        auto dir = scratchDir ("validator");
        auto empty = dir.getChildFile ("empty.wav"); empty.create();
        expect (! AudioFileValidator::validate (empty).ok);
        auto text = dir.getChildFile ("fake.wav"); text.replaceWithText (juce::String::repeatedString ("not audio ", 200));
        expect (! AudioFileValidator::validate (text).ok);
        auto truncatedFile = dir.getChildFile ("trunc.wav");
        juce::MemoryBlock mb; f.loadFileAsData (mb);
        truncatedFile.replaceWithData (mb.getData(), 60);
        expect (! AudioFileValidator::validate (truncatedFile).ok);
        expect (! AudioFileValidator::validate (dir.getChildFile ("missing.wav")).ok);
    }
};

class ControllerTests final : public juce::UnitTest
{
public:
    ControllerTests() : juce::UnitTest ("Generation controller (mock)", "Generation") {}

    struct Rig
    {
        juce::File folder = scratchDir ("generated");
        std::atomic<bool> licensed { true };
        std::atomic<int> completed { 0 };
        std::unique_ptr<GenerationController> controller;

        explicit Rig (int failPercent = 0)
        {
            GenerationController::Dependencies d;
            d.isLicensed = [this] { return licensed.load(); };
            d.generatedFolder = [this] { return folder; };
            d.tempFolder = [this] { return folder.getChildFile ("tmp"); };
            d.onCompleted = [this] (const GenerationResult&) { ++completed; };
            controller = std::make_unique<GenerationController> (d);
            MockAudioGenerationProvider::Options o;
            o.latencyScale = 0.05;
            o.failurePercent = failPercent;
            controller->setProvider (std::make_shared<MockAudioGenerationProvider> (o));
        }

        bool waitForTerminal (int ms)
        {
            return waitUntil ([this] { return ! controller->isBusy() && isTerminal (controller->getCurrentJob().state); }, ms);
        }
    };

    void runTest() override
    {
        beginTest ("Mock generation completes and stores a valid WAV");
        {
            Rig rig;
            auto req = basicRequest ("dark distorted female vocal chop", GenerationType::Vocal);
            req.bpm = 142.0; req.key = "F#"; req.mode = "minor";
            const auto r = rig.controller->enqueue (req);
            expect (r.ok, r.error.technicalDetail);
            expect (rig.waitForTerminal (20000));
            const auto job = rig.controller->getCurrentJob();
            expect (job.state == JobState::Complete, toString (job.state) + " " + job.error.technicalDetail);
            const auto result = rig.controller->getLastResult();
            expect (result.has_value());
            if (result)
            {
                expect (result->localFile.existsAsFile());
                expect (result->localFile.getFileName().startsWith ("Context_142BPM_FSharpMinor_Vocal_"), result->localFile.getFileName());
                expect (AudioFileValidator::validate (result->localFile).ok);
                expect (result->durationSeconds > 0.1);
            }
            expectEquals (rig.completed.load(), 1);
            expectEquals (rig.folder.getChildFile ("tmp").findChildFiles (juce::File::findFiles, false).size(), 0);
        }

        beginTest ("Connection test reports mock ready");
        {
            Rig rig;
            rig.controller->testConnection();
            expect (waitUntil ([&] { return rig.controller->getConnectionResult().status == ConnectionStatus::MockReady; }, 5000));
            expect (rig.controller->getCapabilities().has (Feature::Variation));
        }

        beginTest ("Simulated failure surfaces a friendly error");
        {
            Rig rig;
            rig.controller->enqueue (basicRequest ("kick #fail"));
            expect (rig.waitForTerminal (20000));
            const auto job = rig.controller->getCurrentJob();
            expect (job.state == JobState::Failed);
            expect (job.error.code == ErrorCode::GenerationFailed);
            expect (job.error.userMessage.isNotEmpty());
            expectEquals (rig.completed.load(), 0);
        }

        beginTest ("Cancel stops an active job");
        {
            Rig rig;
            rig.controller->enqueue (basicRequest ("pad #slow"));
            expect (waitUntil ([&] { return isActive (rig.controller->getCurrentJob().state) && rig.controller->getCurrentJob().remoteJobId.isNotEmpty(); }, 5000));
            rig.controller->cancelActive();
            expect (rig.waitForTerminal (5000));
            expect (rig.controller->getCurrentJob().state == JobState::Cancelled);
        }

        beginTest ("Timeout");
        {
            Rig rig;
            rig.controller->setGenerationTimeoutSeconds (2);
            rig.controller->enqueue (basicRequest ("pad #timeout"));
            expect (rig.waitForTerminal (15000));
            expect (rig.controller->getCurrentJob().error.code == ErrorCode::Timeout);
        }

        beginTest ("Rapid Generate presses are bounded; unlicensed and empty prompts refused");
        {
            Rig rig;
            int accepted = 0;
            for (int i = 0; i < 25; ++i)
                accepted += rig.controller->enqueue (basicRequest ("kick")).ok ? 1 : 0;
            expect (accepted >= 3 && accepted <= GenerationController::maxQueued + 1, "accepted " + juce::String (accepted));
            expect (rig.waitForTerminal (60000));
            expectEquals (rig.completed.load(), accepted);

            rig.licensed = false;
            expect (rig.controller->enqueue (basicRequest ("kick")).error.code == ErrorCode::NotLicensed);
            rig.licensed = true;
            expect (rig.controller->enqueue (basicRequest ("")).error.code == ErrorCode::InvalidRequest);
        }

        beginTest ("Destroying the controller mid-job is safe and fast");
        {
            auto rig = std::make_unique<Rig>();
            rig->controller->enqueue (basicRequest ("texture #slow"));
            rig->controller->enqueue (basicRequest ("texture"));
            juce::Thread::sleep (100);
            const auto start = juce::Time::getMillisecondCounter();
            rig.reset();
            expect (juce::Time::getMillisecondCounter() - start < 3000);
        }
    }
};

static ModelTests modelTests;
static BridgeProfileTests bridgeProfileTests;
static BridgeAdapterTests bridgeAdapterTests;
static SynthTests synthTests;
static ControllerTests controllerTests;
} // namespace context::test
