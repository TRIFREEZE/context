#include "TestSupport.h"
#include "DSP/EffectChain.h"
#include "DSP/Effects/DynamicsEffects.h"
#include "DSP/Effects/EqFilterEffects.h"

namespace context::test
{
namespace
{
    constexpr double testRate = 48000.0;
    constexpr int testBlock = 512;

    /** Drum hits + sustained chord + quiet gaps, different on L and R, peaks above 0 dBFS. */
    juce::AudioBuffer<float> richSignal (int numSamples, double sr = testRate)
    {
        juce::AudioBuffer<float> b (2, numSamples);
        juce::Random rng (42);
        const double twoPi = 2.0 * juce::MathConstants<double>::pi;
        for (int i = 0; i < numSamples; ++i)
        {
            const double t = i / sr;
            const double beat = std::fmod (t, 0.25);
            const double inGap = std::fmod (t, 1.0) > 0.55 && std::fmod (t, 1.0) < 0.8;
            const double kick = std::sin (twoPi * 55.0 * beat) * std::exp (-beat * 18.0) * 1.3;
            const double noise = (rng.nextFloat() * 2.0 - 1.0) * std::exp (-beat * 60.0) * 0.6;
            const double chord = 0.06 * (std::sin (twoPi * 220.0 * t) + std::sin (twoPi * 277.2 * t) + std::sin (twoPi * 329.6 * t));
            double l = kick + noise + chord, r = 0.8 * kick + 0.9 * noise + chord + 0.05 * std::sin (twoPi * 440.0 * t + 1.0);
            if (inGap) { l = 1.0e-4 * rng.nextFloat(); r = 1.0e-4 * rng.nextFloat(); }
            b.setSample (0, i, (float) l);
            b.setSample (1, i, (float) r);
        }
        return b;
    }

    juce::AudioBuffer<float> sine (double freq, int n, float amp = 0.25f, double sr = testRate)
    {
        juce::AudioBuffer<float> b (2, n);
        for (int i = 0; i < n; ++i)
        {
            const float v = amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / sr);
            b.setSample (0, i, v);
            b.setSample (1, i, v);
        }
        return b;
    }

    void runEffect (fx::EffectProcessor& e, juce::AudioBuffer<float>& audio, int block = testBlock, double bpm = 120.0)
    {
        fx::ProcessContext ctx;
        ctx.bpm = bpm;
        for (int start = 0; start < audio.getNumSamples(); start += block)
        {
            const int n = juce::jmin (block, audio.getNumSamples() - start);
            juce::AudioBuffer<float> view (audio.getArrayOfWritePointers(), 2, start, n);
            e.process (view, n, ctx);
        }
    }

    double rmsDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        double s = 0.0, ref = 1.0e-12;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
            {
                const double d = a.getSample (ch, i) - b.getSample (ch, i);
                s += d * d;
                ref += (double) a.getSample (ch, i) * a.getSample (ch, i);
            }
        return std::sqrt (s / ref);
    }

    double rms (const juce::AudioBuffer<float>& b, int ch, int start, int len)
    {
        double s = 0.0;
        for (int i = start; i < start + len; ++i) s += (double) b.getSample (ch, i) * b.getSample (ch, i);
        return std::sqrt (s / juce::jmax (1, len));
    }

    bool allFinite (const juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (! std::isfinite (b.getSample (ch, i)))
                    return false;
        return true;
    }

    /** Settings that make otherwise-neutral controls audible (e.g. an EQ band's
        frequency does nothing while its gain is 0 dB). */
    void prime (fx::EffectProcessor& e, const juce::String& testing)
    {
        if (e.typeId() == "eq")
            for (int b = 1; b <= fx::EqEffect::numBands; ++b)
                if (testing != "b" + juce::String (b) + "gain")
                    if (auto* g = e.parameter ("b" + juce::String (b) + "gain"))
                        g->set (9.0f);
    }
}

class EffectsTests final : public juce::UnitTest
{
public:
    EffectsTests() : juce::UnitTest ("Effects and chain", "V2") {}

    juce::AudioBuffer<float> render (const juce::String& type, const juce::var& cfg, const juce::String& paramId, float value)
    {
        auto e = fx::EffectRegistry::builtIn().create (type);
        e->prepare (testRate, testBlock);
        e->setParameterState (cfg);
        prime (*e, paramId);
        if (auto* p = e->parameter (paramId))
            p->set (value);
        e->reset();
        auto audio = richSignal ((int) (testRate * 2.5));   // long enough for 1/1-note delays at 120 BPM
        runEffect (*e, audio);
        return audio;
    }

    void runTest() override
    {
        const auto& reg = fx::EffectRegistry::builtIn();

        beginTest ("Registry: all effects present, unique, creatable");
        {
            expectEquals ((int) reg.types().size(), 20);
            juce::StringArray ids;
            for (auto& t : reg.types())
            {
                expect (! ids.contains (t.id), "duplicate id " + t.id);
                ids.add (t.id);
                auto e = reg.create (t.id);
                expect (e != nullptr && e->typeId() == t.id, t.id);
                expect (e != nullptr && ! e->parameters().empty(), t.id + " has parameters");
                expect (t.shortName.isNotEmpty() && t.category.isNotEmpty() && t.description.isNotEmpty());
            }
            for (auto core : { "utility", "eq", "compressor", "saturation", "clipper", "reverb", "delay", "transient" })
                expect (ids.contains (core), juce::String ("core effect ") + core);
            expect (reg.create ("does-not-exist") == nullptr);
        }

        beginTest ("Every effect: finite output, sane level, at 44.1/48/96 kHz and odd block sizes");
        for (auto& t : reg.types())
            for (double sr : { 44100.0, 48000.0, 96000.0 })
            {
                auto e = reg.create (t.id);
                e->prepare (sr, 333);
                e->reset();
                auto audio = richSignal ((int) (sr * 0.5), sr);
                runEffect (*e, audio, 333);
                expect (allFinite (audio), t.id + " produced NaN/Inf @" + juce::String (sr));
                expect (audio.getMagnitude (0, audio.getNumSamples()) < 16.0f, t.id + " level exploded");
                // Silence in -> silence out (after tails), no self-oscillation or DC.
                juce::AudioBuffer<float> silence (2, (int) (sr * 6.0));
                silence.clear();
                runEffect (*e, silence, 333);
                const float tail = silence.getMagnitude (0, (int) (sr * 5.5), (int) (sr * 0.5));
                expect (tail < 1.0e-3f || t.id == "reverb", t.id + " keeps ringing: " + juce::String (tail));
            }

        beginTest ("Every visible control changes the audio (no cosmetic knobs)");
        for (auto& t : reg.types())
        {
            auto probe = reg.create (t.id);
            const auto defaults = probe->getParameterState();
            for (auto& p : probe->parameters())
            {
                // Find a configuration in which this control is relevant (e.g. Delay "Time" needs Sync off).
                juce::var cfg = juce::JSON::parse (juce::JSON::toString (defaults));
                bool relevant = probe->isParameterRelevant (p->id());
                if (! relevant)
                {
                    // Try every value of one, then two, switch-like controls (e.g. EQ band On + Type).
                    juce::Array<fx::EffectParameter*> switches;
                    for (auto& q : probe->parameters())
                        if (q.get() != p.get() && ! q->isContinuous())
                            switches.add (q.get());
                    auto tryConfig = [&] (std::vector<std::pair<fx::EffectParameter*, int>> set)
                    {
                        auto e = reg.create (t.id);
                        e->setParameterState (defaults);
                        for (auto& [q, v] : set) e->parameter (q->id())->set ((float) v);
                        if (! e->isParameterRelevant (p->id()))
                            return false;
                        for (auto& [q, v] : set) cfg.getDynamicObject()->setProperty (q->id(), (float) v);
                        return true;
                    };
                    for (int a = 0; a < switches.size() && ! relevant; ++a)
                        for (int va = 0; va <= (int) switches[a]->spec().maxValue && ! relevant; ++va)
                        {
                            relevant = tryConfig ({ { switches[a], va } });
                            for (int b = a + 1; b < switches.size() && ! relevant; ++b)
                                for (int vb = 0; vb <= (int) switches[b]->spec().maxValue && ! relevant; ++vb)
                                    relevant = tryConfig ({ { switches[a], va }, { switches[b], vb } });
                        }
                }
                expect (relevant, t.id + "." + p->id() + " is never relevant");

                juce::Array<float> values;
                if (p->isContinuous()) values = { p->spec().minValue, p->spec().maxValue };
                else for (int v = 0; v <= (int) p->spec().maxValue; ++v) values.add ((float) v);

                const auto base = render (t.id, cfg, p->id(), values[0]);
                for (int k = 1; k < values.size(); ++k)
                {
                    const auto other = render (t.id, cfg, p->id(), values[k]);
                    const double d = rmsDiff (base, other);
                    expect (d > 1.0e-4, t.id + "." + p->id() + " = " + juce::String (values[k]) + " sounds identical to " + juce::String (values[0])
                                        + " (diff " + juce::String (d, 8) + ")");
                }
            }
        }

        beginTest ("Parameter values: clamp, snap, text round trip, state round trip");
        {
            auto e = reg.create ("compressor");
            auto* ratio = e->parameter ("ratio");
            ratio->set (1000.0f);
            expectEquals (ratio->get(), 20.0f);
            ratio->set (std::numeric_limits<float>::quiet_NaN());
            expectEquals (ratio->get(), ratio->spec().defaultValue);
            auto* det = e->parameter ("detector");
            det->set (0.7f);
            expectEquals (det->get(), 1.0f);
            expectEquals (det->currentText(), juce::String ("RMS"));
            auto* attack = e->parameter ("attack");
            expect (attack->fromText ("25 ms").has_value() && std::abs (*attack->fromText ("25 ms") - 25.0f) < 1e-4f);
            expect (! attack->fromText ("fast").has_value());
            auto* eqFreq = reg.create ("eq")->parameter ("b3freq");
            expect (eqFreq != nullptr);
            expectEquals (std::round (*reg.create ("eq")->parameter ("b3freq")->fromText ("2.5k")), 2500.0f);
            for (auto& p : e->parameters())
            {
                expect (std::abs (p->fromNormalised (p->toNormalised (p->get())) - p->get()) < 1e-3f * juce::jmax (1.0f, std::abs (p->get())), p->id());
                expect (p->currentText().isNotEmpty());
            }

            e->parameter ("threshold")->set (-33.0f);
            const auto state = e->getParameterState();
            auto e2 = reg.create ("compressor");
            e2->setParameterState (state);
            expectEquals (e2->parameter ("threshold")->get(), -33.0f);
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("threshold", "not a number");
            obj->setProperty ("unknownParam", 5);
            obj->setProperty ("ratio", 999);
            e2->setParameterState (juce::var (obj));
            expectEquals (e2->parameter ("threshold")->get(), -33.0f);
            expectEquals (e2->parameter ("ratio")->get(), 20.0f);
            e2->resetParameters();
            expect (e2->parameter ("threshold")->isDefault());
        }

        beginTest ("DSP correctness: compressor curve, limiter ceiling, EQ gain, synced delay, gate");
        {
            expectWithinAbsoluteError (fx::CompressorEffect::gainComputerDb (-10.0f, -20.0f, 4.0f, 0.0f), -17.5f, 1e-4f);
            expectWithinAbsoluteError (fx::CompressorEffect::gainComputerDb (-30.0f, -20.0f, 4.0f, 6.0f), -30.0f, 1e-4f);
            expect (fx::CompressorEffect::gainComputerDb (-20.0f, -20.0f, 4.0f, 6.0f) < -20.0f, "soft knee already compresses at threshold");

            auto lim = reg.create ("limiter");
            lim->prepare (testRate, testBlock);
            lim->parameter ("ceiling")->set (-6.0f);
            lim->parameter ("drive")->set (12.0f);
            lim->reset();
            auto hot = richSignal ((int) testRate);
            runEffect (*lim, hot);
            expect (hot.getMagnitude (0, hot.getNumSamples()) <= juce::Decibels::decibelsToGain (-6.0f) + 1e-5f, "limiter never exceeds its ceiling");
            expect (lim->getLatencySamples() == 72, "1.5 ms look-ahead at 48 kHz = 72 samples");
            expect (lim->getGainReductionDb().value_or (0.0f) < -1.0f);

            auto eq = reg.create ("eq");
            eq->prepare (testRate, testBlock);
            eq->parameter ("b3type")->set (0);
            eq->parameter ("b3freq")->set (1000.0f);
            eq->parameter ("b3gain")->set (12.0f);
            eq->parameter ("b3q")->set (1.0f);
            eq->reset();
            auto tone = sine (1000.0, 48000, 0.1f);
            runEffect (*eq, tone);
            expectWithinAbsoluteError (juce::Decibels::gainToDecibels (rms (tone, 0, 24000, 24000) / (0.1 / std::sqrt (2.0))), 12.0, 0.3);
            expectWithinAbsoluteError ((double) static_cast<fx::EqEffect*> (eq.get())->responseDb (1000.0f), 12.0, 0.1);

            auto dl = reg.create ("delay");
            dl->prepare (testRate, testBlock);
            dl->parameter ("sync")->set (1);
            dl->parameter ("division")->set (4);   // 1/4 note
            dl->parameter ("mix")->set (100.0f);
            dl->parameter ("feedback")->set (0.0f);
            dl->parameter ("lowcut")->set (20.0f);
            dl->parameter ("highcut")->set (20000.0f);
            dl->reset();
            juce::AudioBuffer<float> click (2, 48000);
            click.clear();
            click.setSample (0, 100, 1.0f);
            click.setSample (1, 100, 1.0f);
            fx::ProcessContext ctx;
            ctx.bpm = 120.0;   // 1/4 note = 0.5 s = 24000 samples
            for (int s = 0; s < 48000; s += 512)
            {
                juce::AudioBuffer<float> view (click.getArrayOfWritePointers(), 2, s, juce::jmin (512, 48000 - s));
                dl->process (view, view.getNumSamples(), ctx);
            }
            int peakAt = 0;
            for (int i = 0; i < 48000; ++i)
                if (std::abs (click.getSample (0, i)) > std::abs (click.getSample (0, peakAt))) peakAt = i;
            expect (std::abs (peakAt - 24100) <= 2, "echo at 0.5 s: got sample " + juce::String (peakAt));

            auto gate = reg.create ("gate");
            gate->prepare (testRate, testBlock);
            gate->parameter ("release")->set (20.0f);
            gate->reset();
            auto gated = richSignal ((int) testRate);
            runEffect (*gate, gated);
            const double gapLevel = rms (gated, 0, (int) (0.70 * testRate), 2400);
            expect (gapLevel < 1.0e-6, "gate closes in the gaps: " + juce::String (gapLevel));
        }

        beginTest ("Chain: add/remove/move/duplicate/bypass/reset and order IS the signal flow");
        {
            fx::EffectChain chain;
            chain.prepare (testRate, testBlock);
            int changes = 0;
            chain.onChanged = [&] { ++changes; };
            const auto distId = chain.add ("distortion");
            const auto filterId = chain.add ("filter");
            expect (distId.isNotEmpty() && filterId.isNotEmpty());
            expectEquals (chain.size(), 2);
            expect (chain.add ("nonexistent").isEmpty());
            chain.effect (1)->parameter ("cutoff")->set (600.0f);
            chain.effect (0)->parameter ("drive")->set (36.0f);

            auto process = [&] (fx::EffectChain& c)
            {
                c.requestReset();
                auto audio = sine (220.0, 24000, 0.5f);
                fx::ProcessContext ctx;
                for (int s = 0; s < audio.getNumSamples(); s += testBlock)
                {
                    juce::AudioBuffer<float> view (audio.getArrayOfWritePointers(), 2, s, juce::jmin (testBlock, audio.getNumSamples() - s));
                    c.process (view, view.getNumSamples(), ctx);
                }
                return audio;
            };
            const auto distThenFilter = process (chain);
            expect (chain.move (0, 1));
            expectEquals (chain.instanceId (0), filterId);
            const auto filterThenDist = process (chain);
            expect (rmsDiff (distThenFilter, filterThenDist) > 0.05, "reordering changes the sound");
            // Distortion last re-creates harmonics above the filter cutoff: more high-frequency energy.
            expect (chain.move (1, 0));
            expect (rmsDiff (distThenFilter, process (chain)) < 1.0e-5, "moving back restores the original order exactly");

            const auto dupId = chain.duplicate (1);
            expectEquals (chain.size(), 3);
            expectEquals (chain.effect (2)->parameter ("cutoff")->get(), 600.0f);
            expect (dupId != filterId);
            expect (chain.remove (2));
            expect (! chain.remove (7));
            expect (chain.resetEffect (1));
            expect (chain.effect (1)->parameter ("cutoff")->isDefault());
            expect (changes >= 6);

            // Bypassing every (latency-free) effect gives back the input exactly.
            fx::EffectChain plain;
            plain.prepare (testRate, testBlock);
            plain.add ("compressor");
            plain.add ("eq");
            plain.setBypassed (0, true);
            plain.setBypassed (1, true);
            juce::AudioBuffer<float> in = sine (330.0, 4096, 0.3f), out (in);
            fx::ProcessContext ctx;
            plain.requestReset();
            plain.process (out, 4096, ctx);   // larger than the prepared block: processed in chunks
            expect (rmsDiff (in, out) < 1.0e-7, "fully bypassed chain is transparent");
            expect (plain.isBypassed (0));
            chain.clear();
            expectEquals (chain.size(), 0);
            for (int i = 0; i < fx::EffectChain::maxEffects + 3; ++i)
                chain.add ("utility");
            expectEquals (chain.size(), fx::EffectChain::maxEffects);
            chain.collectGarbage();
        }

        beginTest ("Chain: bypass is click-free, latency is constant across bypass");
        {
            fx::EffectChain chain;
            chain.prepare (testRate, 256);
            chain.add ("utility");
            chain.effect (0)->parameter ("gain")->set (-24.0f);
            auto audio = sine (100.0, 48000, 0.5f);
            fx::ProcessContext ctx;
            float maxStep = 0.0f;
            float prev = 0.0f;
            for (int s = 0; s < 48000; s += 256)
            {
                if (s == 24064) chain.setBypassed (0, true);
                juce::AudioBuffer<float> view (audio.getArrayOfWritePointers(), 2, s, 256);
                chain.process (view, 256, ctx);
                for (int i = 0; i < 256; ++i)
                {
                    maxStep = juce::jmax (maxStep, std::abs (view.getSample (0, i) - prev));
                    prev = view.getSample (0, i);
                }
            }
            // A 100 Hz sine at 0.5 changes by at most ~0.0066 per sample; a hard switch from -24 dB would jump ~0.47.
            expect (maxStep < 0.02f, "bypass crossfades (max step " + juce::String (maxStep) + ")");

            fx::EffectChain latent;
            latent.prepare (testRate, 256);
            latent.add ("limiter");
            latent.add ("saturation");
            const int L = latent.latencySamples();
            expect (L >= 72, "limiter + oversampling latency reported: " + juce::String (L));
            // Bypassed latent effects still delay the dry signal by the same amount.
            latent.setBypassed (0, true);
            latent.setBypassed (1, true);
            expectEquals (latent.latencySamples(), L);
            juce::AudioBuffer<float> imp (2, 2048);
            imp.clear();
            imp.setSample (0, 10, 1.0f);
            imp.setSample (1, 10, 1.0f);
            latent.process (imp, 2048, ctx);   // first call picks up the snapshot
            imp.clear();
            imp.setSample (0, 10, 1.0f);
            imp.setSample (1, 10, 1.0f);
            latent.requestReset();
            latent.process (imp, 2048, ctx);
            int at = -1;
            for (int i = 0; i < 2048; ++i) if (std::abs (imp.getSample (0, i) - 1.0f) < 1e-6f) at = i;
            expectEquals (at, 10 + L);
        }

        beginTest ("Chain state: round trip, unknown types and junk are tolerated");
        {
            fx::EffectChain a;
            a.add ("eq");
            a.add ("reverb");
            a.effect (1)->parameter ("decay")->set (4.5f);
            a.setBypassed (0, true);
            const auto saved = juce::JSON::toString (a.toVar());
            fx::EffectChain b;
            const auto warnings = b.fromVar (juce::JSON::parse (saved));
            expect (warnings.isEmpty());
            expectEquals (b.size(), 2);
            expectEquals (b.typeId (1), juce::String ("reverb"));
            expectEquals (b.effect (1)->parameter ("decay")->get(), 4.5f);
            expect (b.isBypassed (0) && ! b.isBypassed (1));
            expectEquals (b.instanceId (1), a.instanceId (1));

            auto withUnknown = juce::JSON::parse (R"({"v":1,"fx":[{"type":"granular","params":{}},{"type":"delay","params":{"feedback":500,"bogus":1}},"junk",{"type":"compressor","id":"../../x","params":"nope"}]})");
            const auto w2 = b.fromVar (withUnknown);
            expectEquals (b.size(), 2);
            expect (w2.size() >= 1 && w2[0].contains ("granular"));
            expectEquals (b.effect (0)->parameter ("feedback")->get(), 95.0f);
            expect (b.instanceId (1).containsOnly ("abcdefABCDEF0123456789-"));
            b.fromVar (juce::var ("garbage"));
            expectEquals (b.size(), 0);
            auto clone = a.clone();
            expectEquals (clone->size(), 2);
            expect (clone->effect (0) != a.effect (0), "clone has its own instances");
        }
    }
};

static EffectsTests effectsTests;
} // namespace context::test
