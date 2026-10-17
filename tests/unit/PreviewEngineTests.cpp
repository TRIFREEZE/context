#include "TestSupport.h"
#include "Audio/PreviewEngine.h"

namespace context::test
{
namespace
{
    std::shared_ptr<const juce::AudioBuffer<float>> constant (int n, float v)
    {
        auto b = std::make_shared<juce::AudioBuffer<float>> (2, n);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
                b->setSample (ch, i, v);
        return b;
    }
    std::shared_ptr<const juce::AudioBuffer<float>> ramp (int n, float offset)
    {
        auto b = std::make_shared<juce::AudioBuffer<float>> (2, n);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
                b->setSample (ch, i, offset + (float) i * 1.0e-5f);
        return b;
    }
    std::unique_ptr<PreviewEngine::Program> program (std::shared_ptr<const juce::AudioBuffer<float>> c, std::shared_ptr<const juce::AudioBuffer<float>> o, double sr = 48000.0)
    {
        auto p = std::make_unique<PreviewEngine::Program>();
        p->context = std::move (c);
        p->output = std::move (o);
        p->sampleRate = sr;
        return p;
    }
    juce::AudioBuffer<float> block (PreviewEngine& e, fx::EffectChain& chain, int n, bool offline = false)
    {
        juce::AudioBuffer<float> b (2, n);
        b.clear();
        e.renderAdd (b, n, offline, chain);
        return b;
    }
}

class PreviewEngineTests final : public juce::UnitTest
{
public:
    PreviewEngineTests() : juce::UnitTest ("Two-lane preview engine", "V2") {}

    void runTest() override
    {
        beginTest ("Both lanes play together; modes, mute, solo, gain");
        {
            PreviewEngine e;
            fx::EffectChain chain;
            e.prepare (48000.0, 512);
            chain.prepare (48000.0, 512);
            e.setProgram (program (constant (48000, 0.1f), constant (48000, 0.2f)));
            e.play();
            block (e, chain, 512);                      // start ramp
            auto b = block (e, chain, 512);
            expectWithinAbsoluteError (b.getSample (0, 100), 0.3f, 1e-5f);
            expectWithinAbsoluteError (b.getSample (1, 100), 0.3f, 1e-5f);

            auto settle = [&] { for (int i = 0; i < 10; ++i) block (e, chain, 512); return block (e, chain, 512).getSample (0, 256); };
            e.setMode (PreviewEngine::Mode::Context);  expectWithinAbsoluteError (settle(), 0.1f, 1e-4f);
            e.setMode (PreviewEngine::Mode::Output);   expectWithinAbsoluteError (settle(), 0.2f, 1e-4f);
            e.setMode (PreviewEngine::Mode::Both);     expectWithinAbsoluteError (settle(), 0.3f, 1e-4f);
            e.setLaneMute (PreviewEngine::contextLane, true);  expectWithinAbsoluteError (settle(), 0.2f, 1e-4f);
            e.setLaneMute (PreviewEngine::contextLane, false);
            e.setLaneSolo (PreviewEngine::contextLane, true);  expectWithinAbsoluteError (settle(), 0.1f, 1e-4f);
            e.setLaneSolo (PreviewEngine::contextLane, false);
            e.setLaneGainDb (PreviewEngine::outputLane, -6.0206f); expectWithinAbsoluteError (settle(), 0.2f, 1e-4f);
            e.setLaneGainDb (PreviewEngine::outputLane, 0.0f);
            e.setMasterGainDb (-6.0206f);              expectWithinAbsoluteError (settle(), 0.15f, 1e-4f);
            e.collectGarbage();
        }

        beginTest ("Shared sample-counted timeline: shorter lane repeats, loop wraps without drift");
        {
            PreviewEngine e;
            fx::EffectChain chain;
            e.prepare (48000.0, 256);
            chain.prepare (48000.0, 256);
            e.setProgram (program (ramp (1000, 0.0f), ramp (400, 1.0f)));
            e.play();
            juce::int64 timelinePos = 0;
            int checked = 0;
            for (int blk = 0; blk < 40; ++blk)
            {
                const int n = 37 + (blk * 53) % 211;   // odd, varying block sizes
                auto b = block (e, chain, n);
                for (int i = 0; i < n; ++i, ++timelinePos)
                {
                    if (timelinePos < 2000) continue;   // after the start ramp
                    const int p = (int) (timelinePos % 1000);
                    const float expected = (float) p * 1.0e-5f + 1.0f + (float) (p % 400) * 1.0e-5f;
                    expectWithinAbsoluteError (b.getSample (0, i), expected, 1e-5f);
                    ++checked;
                }
            }
            expect (checked > 1000);
            expectEquals (e.timelineLength(), (juce::int64) 1000);
        }

        beginTest ("Loop off stops at the end; stop fades out; offline renders and wrong rates are silent");
        {
            PreviewEngine e;
            fx::EffectChain chain;
            e.prepare (48000.0, 512);
            chain.prepare (48000.0, 512);
            e.setLooping (false);
            e.setProgram (program (constant (3000, 0.1f), nullptr));
            e.play();
            for (int i = 0; i < 8; ++i) block (e, chain, 512);
            expect (! e.isPlaying(), "stopped at the end of the timeline");
            expectEquals (block (e, chain, 512).getMagnitude (0, 512), 0.0f);

            e.setLooping (true);
            e.play();
            for (int i = 0; i < 4; ++i) block (e, chain, 512);
            e.stop();
            expect (! e.isPlaying());
            auto fade = block (e, chain, 512);
            expect (fade.getSample (0, 0) > 0.05f && std::abs (fade.getSample (0, 511)) < 1e-6f, "stop ramps down within the block");
            expectEquals (block (e, chain, 512).getMagnitude (0, 512), 0.0f);

            e.play();
            expectEquals (block (e, chain, 512, true).getMagnitude (0, 512), 0.0f);

            PreviewEngine wrong;
            wrong.prepare (48000.0, 512);
            wrong.setProgram (program (constant (3000, 0.1f), nullptr, 44100.0));
            wrong.play();
            block (wrong, chain, 512);
            expectEquals (block (wrong, chain, 512).getMagnitude (0, 512), 0.0f, "program at another sample rate is not played");
        }

        beginTest ("Effect latency is compensated: processed output lines up with the context lane");
        {
            PreviewEngine e;
            fx::EffectChain chain;
            e.prepare (48000.0, 512);
            chain.prepare (48000.0, 512);
            chain.add ("limiter");          // 72 samples of look-ahead
            expectEquals (chain.latencySamples(), 72);
            auto ctx = std::make_shared<juce::AudioBuffer<float>> (2, 8000);
            auto out = std::make_shared<juce::AudioBuffer<float>> (2, 8000);
            ctx->clear();
            out->clear();
            for (int ch = 0; ch < 2; ++ch) { ctx->setSample (ch, 3000, 0.25f); out->setSample (ch, 3000, 0.5f); }
            e.setProgram (program (ctx, out));
            e.play();
            juce::AudioBuffer<float> all (2, 8000);
            all.clear();
            for (int s = 0; s < 8000; s += 500)
            {
                auto b = block (e, chain, 500);
                all.copyFrom (0, s, b, 0, 0, 500);
            }
            int peakAt = 0;
            for (int i = 0; i < 8000; ++i) if (all.getSample (0, i) > all.getSample (0, peakAt)) peakAt = i;
            expectEquals (peakAt, 3000);
            expectWithinAbsoluteError (all.getSample (0, 3000), 0.75f, 1e-3f, "both impulses at the same sample");
        }

        beginTest ("Original/Processed compare and program swaps are click-free");
        {
            PreviewEngine e;
            fx::EffectChain chain;
            e.prepare (48000.0, 256);
            chain.prepare (48000.0, 256);
            chain.add ("utility");
            chain.effect (0)->parameter ("gain")->set (-20.0f);
            e.setProgram (program (nullptr, constant (48000, 0.5f)));
            e.play();
            for (int i = 0; i < 20; ++i) block (e, chain, 256);
            expectWithinAbsoluteError (block (e, chain, 256).getSample (0, 100), 0.05f, 1e-4f);
            e.setCompareOriginal (true);
            float maxStep = 0.0f, prev = 0.05f;
            for (int i = 0; i < 20; ++i)
            {
                auto b = block (e, chain, 256);
                for (int k = 0; k < 256; ++k) { maxStep = juce::jmax (maxStep, std::abs (b.getSample (0, k) - prev)); prev = b.getSample (0, k); }
            }
            expectWithinAbsoluteError (prev, 0.5f, 1e-4f, "compare plays the unprocessed source");
            expect (maxStep < 0.01f, "A/B switch is smoothed (max step " + juce::String (maxStep) + ")");

            e.setCompareOriginal (false);
            for (int i = 0; i < 20; ++i) block (e, chain, 256);
            e.setProgram (program (nullptr, constant (48000, -0.5f)));
            maxStep = 0.0f;
            prev = 0.05f;
            for (int i = 0; i < 10; ++i)
            {
                auto b = block (e, chain, 256);
                for (int k = 0; k < 256; ++k) { maxStep = juce::jmax (maxStep, std::abs (b.getSample (0, k) - prev)); prev = b.getSample (0, k); }
            }
            expectWithinAbsoluteError (prev, -0.05f, 1e-4f, "new program playing");
            expect (maxStep < 0.01f, "program swap crossfades (max step " + juce::String (maxStep) + ")");
            e.collectGarbage();
        }
    }
};

static PreviewEngineTests previewEngineTests;
} // namespace context::test
