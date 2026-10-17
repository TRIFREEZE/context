// Loads the BUILT Context.vst3 bundle the way a DAW does (via JUCE's VST3 hosting)
// and exercises it: scan, instantiate, process, check passthrough, save/restore
// state, open/close the editor, multiple instances, destroy.
//
//   ContextHostTest <path to Context.vst3> [--no-editor]
//
// On GitHub Actions this runs against the real Windows VST3 before it is packaged.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

namespace
{
int failures = 0;
void check (bool ok, const juce::String& what)
{
    std::printf ("%s %s\n", ok ? "  ok  " : "  FAIL", what.toRawUTF8());
    if (! ok)
        ++failures;
}

bool passthroughOk (juce::AudioPluginInstance& p, double sr, int block, int numChannels)
{
    p.setPlayConfigDetails (numChannels, numChannels, sr, block);
    p.prepareToPlay (sr, block);
    juce::AudioBuffer<float> buf (numChannels, block), ref;
    juce::MidiBuffer midi;
    juce::Random rng (7);
    bool same = true;
    for (int iter = 0; iter < 50; ++iter)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = 0; i < block; ++i)
                buf.setSample (ch, i, rng.nextFloat() * 1.6f - 0.8f);
        ref.makeCopyOf (buf);
        p.processBlock (buf, midi);
        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = 0; i < block; ++i)
                same = same && std::abs (buf.getSample (ch, i) - ref.getSample (ch, i)) < 1.0e-6f;
    }
    p.releaseResources();
    return same;
}
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::ArgumentList args (argc, argv);
    if (args.size() < 1)
    {
        std::printf ("usage: ContextHostTest <Context.vst3> [--no-editor]\n");
        return 2;
    }
    const auto bundle = juce::File (args[0].text);
    const bool withEditor = ! args.containsOption ("--no-editor");
    std::printf ("Loading %s\n", bundle.getFullPathName().toRawUTF8());
    check (bundle.exists(), "bundle exists");

    juce::AudioPluginFormatManager formats;
    formats.addFormat (std::make_unique<juce::VST3PluginFormat>());
    auto* vst3 = formats.getFormat (0);

    juce::OwnedArray<juce::PluginDescription> found;
    vst3->findAllTypesForFile (found, bundle.getFullPathName());
    check (found.size() == 1, "scan finds exactly one plugin (found " + juce::String (found.size()) + ")");
    if (found.isEmpty())
        return 1;

    const auto& desc = *found[0];
    std::printf ("  name=%s vendor=%s version=%s category=%s\n", desc.name.toRawUTF8(), desc.manufacturerName.toRawUTF8(),
                 desc.version.toRawUTF8(), desc.category.toRawUTF8());
    check (desc.name == "Context", "plugin name is Context");
    check (! desc.isInstrument, "registered as an effect");

    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        juce::String error;
        auto instance = formats.createPluginInstance (desc, sr, 512, error);
        check (instance != nullptr, "instantiate @" + juce::String (sr) + (error.isNotEmpty() ? " (" + error + ")" : juce::String()));
        if (instance == nullptr)
            return 1;
        check (passthroughOk (*instance, sr, 64, 2), "bit-transparent stereo passthrough @" + juce::String (sr) + ", block 64");
        check (passthroughOk (*instance, sr, 4096, 2), "bit-transparent stereo passthrough @" + juce::String (sr) + ", block 4096");
        check (instance->getLatencySamples() == 0, "zero latency");
    }

    // State round trip + editor lifecycle + multiple instances
    juce::String error;
    auto a = formats.createPluginInstance (desc, 48000.0, 256, error);
    auto b = formats.createPluginInstance (desc, 48000.0, 256, error);
    check (a != nullptr && b != nullptr, "two simultaneous instances");
    if (a == nullptr || b == nullptr)
        return 1;

    juce::MemoryBlock state;
    a->getStateInformation (state);
    check (state.getSize() > 0 && state.getSize() < 16384, "state is small (" + juce::String ((int) state.getSize()) + " bytes)");
    b->setStateInformation (state.getData(), (int) state.getSize());
    const char junk[] = "garbage state from a corrupted project";
    b->setStateInformation (junk, (int) sizeof (junk));
    check (passthroughOk (*b, 48000.0, 256, 2), "still transparent after loading garbage state");

    if (withEditor && a->hasEditor())
    {
        for (int i = 0; i < 3; ++i)
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (a->createEditor());
            check (ed != nullptr, "editor opens (" + juce::String (i + 1) + ")");
            juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
            check (passthroughOk (*a, 48000.0, 256, 2), "transparent while the editor is open");
        }
    }

    a.reset();
    b.reset();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    check (true, "instances destroyed cleanly");

    std::printf ("\n==== host test: %s (%d failure%s) ====\n", failures == 0 ? "PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
