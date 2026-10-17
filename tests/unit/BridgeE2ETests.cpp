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

    /** 8 s of a 120 BPM pulse (4 bars), the shape of Context's captured track context. */
    static bool writeTrackContext (const juce::File& f)
    {
        const double sr = 44100.0;
        juce::AudioBuffer<float> b (2, (int) (sr * 8.0));
        b.clear();
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double t = std::fmod (i / sr, 0.5);
            const float v = t < 0.08 ? (float) (0.6 * std::sin (2.0 * juce::MathConstants<double>::pi * 110.0 * t) * (1.0 - t / 0.08)) : 0.0f;
            b.setSample (0, i, v);
            b.setSample (1, i, v);
        }
        f.deleteFile();
        std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
        if (os == nullptr) return false;
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 2, 16, {}, 0));
        if (w == nullptr) return false;
        os.release();
        return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    Outcome generate (const juce::String& url, const juce::String& prompt, GenerationType type,
                      std::optional<double> bpm = {}, juce::String key = {}, juce::String mode = {},
                      juce::String contextMode = {})
    {
        auto folder = scratchDir ("kie-bridge-e2e");
        GenerationController::Dependencies d;
        d.isLicensed = [] { return true; };
        d.generatedFolder = [folder] { return folder; };
        d.tempFolder = [folder] { return folder.getChildFile ("tmp"); };
        d.makeAudioContext = [folder] (const juce::String& id) -> std::optional<AudioConditioningPayload>
        {
            AudioConditioningPayload a;
            folder.getChildFile ("tmp").createDirectory();
            a.wavFile = folder.getChildFile ("tmp").getChildFile (id + "-context.wav");
            if (! writeTrackContext (a.wavFile)) return std::nullopt;
            a.seconds = 8.0;
            a.sampleRate = 44100.0;
            a.bars = 4;
            return a;
        };
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
        if (contextMode.isNotEmpty())
        {
            r.audioContext = AudioConditioningPayload{};
            r.contextMode = contextMode;
        }
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

        beginTest ("Send my track: Context's provider uploads the track through the bridge (layer and restyle)");
        {
            // A fresh provider whose cached capabilities are the profile defaults (no audio):
            // the controller must re-check the bridge and then send the track.
            // The separated vocal stem comes back from Kie.ai as MP3; the bridge needs its optional
            // decoder for that (run_kie_bridge_e2e.py says whether it is installed).
            const bool mp3 = juce::SystemStats::getEnvironmentVariable ("CONTEXT_E2E_MP3", "1") == "1";
            const auto layer = generate (url, "female vocal hook", GenerationType::Vocal, 120.0, "A", "minor", "layer");
            if (mp3)
            {
                expect (layer.job.state == JobState::Complete, toString (layer.job.state) + " " + layer.job.error.userMessage + " " + layer.job.error.technicalDetail);
                expect (layer.result.has_value() && layer.result->metadata.getWithDefault ("contextMode", "").toString() == "layer");
                if (layer.result)
                    expect (AudioFileValidator::validate (layer.result->localFile).ok);
            }
            else
            {
                expect (layer.job.state == JobState::Failed && layer.job.error.userMessage.contains ("MP3 decoder"),
                        "without the decoder the layer job explains how to install it: " + layer.job.error.userMessage);
            }

            const auto cover = generate (url, "lofi piano version", GenerationType::Melody, 120.0, {}, {}, "cover");
            expect (cover.job.state == JobState::Complete, cover.job.error.userMessage + " " + cover.job.error.technicalDetail);
            expect (cover.result.has_value() && cover.result->metadata.getWithDefault ("contextMode", "").toString() == "cover");
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
