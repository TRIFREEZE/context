// Renders the main screens in typical states to PNG files (for visual review).
// Runs only with:  ContextTests --category=Screens
#include "TestSupport.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace context::test
{
class ScreenshotTests final : public juce::UnitTest
{
public:
    ScreenshotTests() : juce::UnitTest ("UI screenshots", "Screens") {}

    void snap (juce::AudioProcessorEditor& ed, const juce::String& name)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        const auto img = ed.createComponentSnapshot (ed.getLocalBounds());
        auto out = isolatedRoot().getChildFile ("screen-" + name + ".png");
        juce::FileOutputStream os (out);
        juce::PNGImageFormat().writeImageToStream (img, os);
        logMessage ("  snapshot: " + out.getFullPathName());
    }

    void runTest() override
    {
        beginTest ("Workspace states");
        ContextAudioProcessor p;
        p.prepareToPlay (48000.0, 512);
        p.services().settings().update ([] (Settings& st) { st.firstRunComplete = true; });
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        ed->setSize (1120, 800);
        snap (*ed, "empty");

        p.session().startCapture();
        snap (*ed, "armed");
        p.session().cancelCapture();

        for (auto type : { "eq", "compressor", "delay", "reverb", "transient" })
        {
            p.effects().clear();
            p.effects().add (type);
            if (juce::String (type) == "eq")
            {
                p.effects().effect (0)->parameter ("b3gain")->set (6.0f);
                p.effects().effect (0)->parameter ("b1on")->set (1.0f);
                p.effects().effect (0)->parameter ("b5gain")->set (-4.0f);
            }
            if (auto* e = dynamic_cast<ContextAudioProcessorEditor*> (ed.get()))
                e->showTab ("generate");
            snap (*ed, juce::String ("fx-") + type);
        }
        if (auto* e = dynamic_cast<ContextAudioProcessorEditor*> (ed.get()))
        {
            e->showTab ("history");
            snap (*ed, "history");
            e->showTab ("settings");
            snap (*ed, "settings");
        }
        expect (true);
    }
};

static ScreenshotTests screenshotTests;
} // namespace context::test
