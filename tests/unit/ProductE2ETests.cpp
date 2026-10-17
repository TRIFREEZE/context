// Full product flow with the REAL compiled-in license public key.
// Not part of CI (it needs the matching private key, which never goes into the repo).
// Run manually:  ContextTests --product-e2e --license-server=http://127.0.0.1:8787 --license-key=CTX-...
#include "TestSupport.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Networking/AudioFileValidator.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace context::test
{
class ProductE2ETests final : public juce::UnitTest
{
public:
    ProductE2ETests() : juce::UnitTest ("Product end-to-end (real public key)", "ProductE2E") {}

    void runTest() override
    {
        const auto server = integrationUrls()["license-server"];
        const auto key = integrationUrls()["license-key"];
        beginTest ("Inputs");
        expect (server.isNotEmpty() && key.isNotEmpty());
        if (server.isEmpty())
            return;

        ContextAudioProcessor p;
        p.prepareToPlay (48000.0, 512);
        auto& services = p.services();
        services.settings().update ([server] (Settings& s) { s.licenseServerUrl = server; s.providerId = "mock"; s.firstRunComplete = true; });

        beginTest ("Activate with the real key pair");
        services.license().activate (key);
        expect (waitUntil ([&] { return services.license().getStatus() != LicenseManager::Status::Activating && services.license().waitForIdle (10); }, 20000));
        expect (p.isLicensed(), services.license().getMessage());

        beginTest ("Mock generation through the processor, result auto-selected");
        expect (waitUntil ([&] { return p.generation().getProvider() != nullptr && p.generation().getProvider()->id() == "mock"; }, 5000));
        auto ui = p.getUiState();
        ui.prompt = "short punchy trap kick";
        ui.type = GenerationType::OneShot;
        p.setUiState (ui);
        const auto r = p.generate();
        expect (r.ok, r.error.technicalDetail);
        expect (waitUntil ([&] { return ! p.generation().isBusy(); }, 30000));
        expect (p.generation().getCurrentJob().state == JobState::Complete, p.generation().getCurrentJob().error.technicalDetail);
        expect (waitUntil ([&] { return p.getSelectedResult().has_value(); }, 5000));
        const auto sel = p.getSelectedResult();
        expect (sel && AudioFileValidator::validate (sel->localFile).ok);

        beginTest ("Regenerate and variation");
        expect (p.regenerate (*sel).ok);
        expect (waitUntil ([&] { return ! p.generation().isBusy(); }, 30000));
        expect (p.variation (*sel).ok);
        expect (waitUntil ([&] { return ! p.generation().isBusy(); }, 30000));
        expect (services.history().size() >= 3);

        beginTest ("Preview through processBlock");
        p.previewResult (*p.getSelectedResult());
        expect (waitUntil ([&] { return p.preview().isPlaying(); }, 5000));
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        float peak = 0;
        for (int i = 0; i < 20; ++i)
        {
            buf.clear();
            p.processBlock (buf, midi);
            peak = juce::jmax (peak, buf.getMagnitude (0, 512));
        }
        expect (peak > 0.05f, "preview should be audible: " + juce::String (peak));

        beginTest ("Screenshot of the licensed product");
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (500);
        const auto img = ed->createComponentSnapshot (ed->getLocalBounds());
        auto out = isolatedRoot().getChildFile ("ui-e2e.png");
        juce::FileOutputStream os (out);
        juce::PNGImageFormat().writeImageToStream (img, os);
        logMessage ("  snapshot: " + out.getFullPathName());
        p.preview().stop();
    }
};

static ProductE2ETests productE2ETests;
} // namespace context::test
