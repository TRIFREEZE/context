#include "TestSupport.h"
#include "PluginProcessor.h"

namespace context::test
{
class ProcessorPassthroughTests final : public juce::UnitTest
{
public:
    ProcessorPassthroughTests() : juce::UnitTest ("Processor passthrough", "Processor") {}

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
                for (int iter = 0; iter < 20; ++iter)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < block; ++i)
                            buf.setSample (ch, i, rng.nextFloat() * 2.0f - 1.0f);
                    ref.makeCopyOf (buf);
                    juce::MidiBuffer midi;
                    p.processBlock (buf, midi);

                    bool same = true;
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < block; ++i)
                            same = same && (buf.getSample (ch, i) == ref.getSample (ch, i));
                    expect (same, "output must equal input");
                }
                p.releaseResources();
            }
    }
};

static ProcessorPassthroughTests processorPassthroughTests;
} // namespace context::test
