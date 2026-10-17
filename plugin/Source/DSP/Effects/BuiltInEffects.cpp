// The one place that lists the effects shipped with Context.
// To add an effect: write the class (derive from EffectProcessor), then add one
// registerEffect(...) line below. The rack, inspector, presets and saved state pick
// it up automatically.
#include "DSP/EffectRegistry.h"
#include "UtilityEffects.h"
#include "EqFilterEffects.h"
#include "DynamicsEffects.h"
#include "SaturationEffects.h"
#include "SpaceEffects.h"
#include "ModulationEffects.h"
#include "LoFiEffects.h"

namespace context::fx
{
namespace
{
    template <typename T>
    std::function<std::unique_ptr<EffectProcessor>()> make()
    {
        return [] { return std::make_unique<T>(); };
    }
}

void registerBuiltInEffects (EffectRegistry& r)
{
    r.registerEffect ({ "eq",         "EQ",           "EQ",     "EQ & Filter", "5-band parametric EQ",                         make<EqEffect>() });
    r.registerEffect ({ "filter",     "Filter",       "FILTER", "EQ & Filter", "Resonant low/high/band-pass filter",           make<FilterEffect>() });
    r.registerEffect ({ "compressor", "Compressor",   "COMP",   "Dynamics",    "Smooth or punchy compression",                 make<CompressorEffect>() });
    r.registerEffect ({ "limiter",    "Limiter",      "LIMIT",  "Dynamics",    "Look-ahead peak limiter",                      make<LimiterEffect>() });
    r.registerEffect ({ "gate",       "Gate",         "GATE",   "Dynamics",    "Cut noise and tails between hits",             make<GateEffect>() });
    r.registerEffect ({ "transient",  "Transient",    "TRANS",  "Dynamics",    "Shape attack and sustain",                     make<TransientEffect>() });
    r.registerEffect ({ "saturation", "Saturation",   "SAT",    "Saturation",  "Tape, tube and warm clip saturation",          make<SaturationEffect>() });
    r.registerEffect ({ "distortion", "Distortion",   "DIST",   "Saturation",  "Hard clip, fold, fuzz and rectify",            make<DistortionEffect>() });
    r.registerEffect ({ "clipper",    "Soft Clipper", "CLIP",   "Saturation",  "Transparent peak control with a soft knee",    make<SoftClipperEffect>() });
    r.registerEffect ({ "reverb",     "Reverb",       "VERB",   "Space",       "Algorithmic room-to-hall reverb",              make<ReverbEffect>() });
    r.registerEffect ({ "delay",      "Delay",        "DELAY",  "Space",       "Tempo-synced stereo / ping-pong delay",        make<DelayEffect>() });
    r.registerEffect ({ "chorus",     "Chorus",       "CHORUS", "Modulation",  "Two-voice stereo chorus",                      make<ChorusEffect>() });
    r.registerEffect ({ "flanger",    "Flanger",      "FLANGE", "Modulation",  "Jet sweep with feedback",                      make<FlangerEffect>() });
    r.registerEffect ({ "phaser",     "Phaser",       "PHASE",  "Modulation",  "Swept all-pass phaser",                        make<PhaserEffect>() });
    r.registerEffect ({ "tremolo",    "Tremolo",      "TREM",   "Modulation",  "Volume modulation, free or synced",            make<TremoloEffect>() });
    r.registerEffect ({ "autopan",    "Auto-Pan",     "PAN",    "Modulation",  "Stereo panning modulation, free or synced",    make<AutoPanEffect>() });
    r.registerEffect ({ "bitcrush",   "Bitcrusher",   "BITS",   "Lo-Fi",       "Bit-depth reduction",                          make<BitcrusherEffect>() });
    r.registerEffect ({ "downsample", "Downsampler",  "RATE",   "Lo-Fi",       "Sample-rate reduction",                        make<DownsamplerEffect>() });
    r.registerEffect ({ "utility",    "Utility",      "UTIL",   "Utility",     "Gain, balance, polarity, mono",                make<UtilityEffect>() });
    r.registerEffect ({ "stereo",     "Stereo",       "WIDTH",  "Utility",     "Width and mono low end",                       make<StereoEffect>() });
}
} // namespace context::fx
