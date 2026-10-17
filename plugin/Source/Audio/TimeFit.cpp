#include "TimeFit.h"
#include "Resampler.h"

namespace context
{
juce::String toId (FitMode m)
{
    switch (m)
    {
        case FitMode::Auto:    return "auto";
        case FitMode::Stretch: return "fit";
        case FitMode::Trim:    return "trim";
        case FitMode::Loop:    return "loop";
        case FitMode::Repitch: return "repitch";
    }
    return "auto";
}

juce::String toLabel (FitMode m)
{
    switch (m)
    {
        case FitMode::Auto:    return "Auto";
        case FitMode::Stretch: return "Fit";
        case FitMode::Trim:    return "Trim";
        case FitMode::Loop:    return "Loop";
        case FitMode::Repitch: return "Repitch";
    }
    return "Auto";
}

juce::String tooltipFor (FitMode m)
{
    switch (m)
    {
        case FitMode::Auto:    return "Chooses Fit, Loop or Trim from the material and how far off it is.";
        case FitMode::Stretch: return "Time-stretch the whole sound to exactly the selected bars.";
        case FitMode::Trim:    return "Cut to the selected bars (shorter sounds are padded with silence).";
        case FitMode::Loop:    return "Repeat to fill the bars; near-bar-length loops are locked to the grid first.";
        case FitMode::Repitch: return "Speed up or slow down like tape: pitch follows tempo.";
    }
    return {};
}

std::optional<FitMode> fitModeFromId (const juce::String& id)
{
    for (auto m : allFitModes)
        if (toId (m) == id)
            return m;
    return std::nullopt;
}

juce::var FitSettings::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("mode", toId (mode));
    o->setProperty ("algorithm", stretch::toId (algorithm));
    o->setProperty ("pitch", pitchSemitones);
    return juce::var (o);
}

FitSettings FitSettings::fromVar (const juce::var& v)
{
    FitSettings s;
    s.mode = fitModeFromId (v.getProperty ("mode", {}).toString()).value_or (FitMode::Auto);
    s.algorithm = stretch::algorithmFromId (v.getProperty ("algorithm", {}).toString()).value_or (stretch::Algorithm::Auto);
    const double p = (double) v.getProperty ("pitch", 0.0);
    s.pitchSemitones = std::isfinite (p) ? juce::jlimit (-24.0, 24.0, p) : 0.0;
    return s;
}

namespace timefit
{
namespace
{
    juce::AudioBuffer<float> stereoAtRate (const AudioAsset& a, double rate)
    {
        auto st = assets::toStereo (a.audio);
        if (std::abs (a.sampleRate - rate) < 0.5)
            return st;
        return dsp::resampler::convertRate (st, a.sampleRate, rate);
    }

    void fadeOutTail (juce::AudioBuffer<float>& b, int length, int fade)
    {
        fade = juce::jmin (fade, length);
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            float* d = b.getWritePointer (ch, length - fade);
            for (int i = 0; i < fade; ++i)
                d[i] *= 1.0f - (float) (i + 1) / (float) fade;
        }
    }

    /** Copy (or pad) to exactly `length`, fading out if the cut lands on sound. */
    juce::AudioBuffer<float> trimTo (const juce::AudioBuffer<float>& in, juce::int64 length, double rate)
    {
        juce::AudioBuffer<float> out (2, (int) length);
        out.clear();
        const int n = (int) juce::jmin<juce::int64> (length, in.getNumSamples());
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, 0, in, juce::jmin (ch, in.getNumChannels() - 1), 0, n);
        if (in.getNumSamples() > length && n > 0)
            if (std::abs (out.getSample (0, n - 1)) > 1.0e-3f || std::abs (out.getSample (1, n - 1)) > 1.0e-3f)
                fadeOutTail (out, n, (int) (0.004 * rate));
        return out;
    }

    juce::AudioBuffer<float> runEngine (stretch::Algorithm algo, const juce::AudioBuffer<float>& in, double ratio, double pitch,
                                        double rate, const stretch::TimeStretchEngine::Progress& progress)
    {
        auto engine = stretch::createEngine (algo);
        engine->prepare (rate, in.getNumChannels());
        engine->setRatio (ratio);
        engine->setPitchSemitones (pitch);
        return engine->process (in, progress);
    }
}

int loopUnitBars (double sourceBars, int targetBars, double tolerance)
{
    if (sourceBars <= 0.0)
        return 0;
    int best = 0;
    double bestErr = 1.0e9;
    for (int unit = 1; unit <= targetBars; ++unit)
    {
        if (targetBars % unit != 0)
            continue;
        const double err = std::abs (sourceBars / unit - 1.0);
        if (err <= tolerance && err < bestErr)
        {
            best = unit;
            bestErr = err;
        }
    }
    return best;
}

FitMode chooseMode (double sourceBars, int targetBars, bool shortSound)
{
    if (shortSound || sourceBars < 0.75)
        return FitMode::Trim;                                   // one-shots sit on bar 1, padded
    const double r = targetBars / sourceBars;
    if (std::abs (r - 1.0) <= 0.005)
        return FitMode::Trim;                                   // already on the grid (sub-0.5%: no stretch needed)
    const int unit = loopUnitBars (sourceBars, targetBars);
    if (unit > 0 && unit < targetBars)
        return FitMode::Loop;                                   // e.g. a 2-bar loop into 8 bars
    if (r >= 0.75 && r <= 1.33)
        return FitMode::Stretch;                                // close: stretch to the grid
    if (r < 0.75)
        return FitMode::Trim;                                   // much longer: take the first bars
    return FitMode::Loop;                                       // much shorter and irregular: repeat
}

std::optional<double> inferSourceBpm (const AudioAsset& a, const musical::Tempo& project, juce::String& origin)
{
    if (a.bpm.has_value() && musical::isValidBpm (*a.bpm))
    {
        origin = a.origin == AudioAsset::Origin::Generated ? "generator" : "file name";
        return a.bpm;
    }
    if (auto fromName = musical::bpmFromFileName (a.displayName))
    {
        origin = "file name";
        return fromName;
    }
    // A loop that is an exact number of bars at a plausible tempo near the project tempo.
    const double secs = a.seconds();
    double bestBpm = 0.0, bestDiff = 1.0e9;
    for (int bars : { 1, 2, 4, 8, 16 })
    {
        const double bpm = musical::bpmForLoop (secs, bars, project.signature);
        if (bpm < 60.0 || bpm > 200.0)
            continue;
        const double diff = std::abs (bpm / project.bpm - 1.0);
        if (diff < 0.15 && diff < bestDiff)
        {
            bestDiff = diff;
            bestBpm = bpm;
        }
    }
    if (bestBpm > 0.0)
    {
        origin = "length";
        return std::round (bestBpm * 100.0) / 100.0;
    }
    return std::nullopt;
}

FitResult render (const FitRequest& req, const stretch::TimeStretchEngine::Progress& progress)
{
    FitResult result;
    if (req.source == nullptr || ! req.source->isValid())
    {
        result.error = "There's no audio to fit.";
        return result;
    }
    if (! req.tempo.isValid() || req.bars <= 0 || ! (req.outputRate > 0.0))
    {
        result.error = "The tempo isn't known yet. Capture context or set a tempo.";
        return result;
    }

    const auto& src = *req.source;
    auto& rep = result.report;
    const double rate = req.outputRate;
    rep.targetBars = req.bars;
    rep.targetSamples = musical::samplesForBars (req.bars, req.tempo, rate);
    rep.outputRate = rate;
    rep.sourceSeconds = src.seconds();
    rep.sourceBars = musical::barsForSeconds (rep.sourceSeconds, req.tempo);
    rep.sourceBpm = inferSourceBpm (src, req.tempo, rep.sourceBpmOrigin);
    const auto target = rep.targetSamples;
    if (target <= 0 || target > (juce::int64) (rate * 600.0))
    {
        result.error = "The selected length is too long at this tempo.";
        return result;
    }

    // Short one-shots never get stretched in Auto (a kick stays a kick on bar 1).
    const bool shortSound = rep.sourceSeconds < 0.6 * musical::secondsForBars (1, req.tempo);
    FitMode mode = req.settings.mode == FitMode::Auto ? chooseMode (rep.sourceBars, req.bars, shortSound) : req.settings.mode;
    rep.modeUsed = mode;

    auto algo = req.settings.algorithm;
    if (algo == stretch::Algorithm::Auto)
        algo = (req.material ? *req.material : stretch::analyseMaterial (src.audio, src.sampleRate)).suggested;
    if (mode == FitMode::Repitch)
        algo = stretch::Algorithm::Repitch;
    if (algo == stretch::Algorithm::Repitch && mode == FitMode::Stretch)
        mode = rep.modeUsed = FitMode::Repitch;
    rep.algorithmUsed = algo;
    const double pitch = algo == stretch::Algorithm::Repitch ? 0.0 : req.settings.pitchSemitones;

    if (progress.cancelled())
        return result;

    juce::AudioBuffer<float> out;
    switch (mode)
    {
        case FitMode::Repitch:
        {
            // One band-limited resample from the source rate straight to the target length.
            auto st = assets::toStereo (src.audio);
            out = dsp::resampler::toLength (st, target);
            rep.engineUsed = true;
            rep.algorithmUsed = stretch::Algorithm::Repitch;
            rep.stretchPercent = 100.0 * ((double) target / rate / rep.sourceSeconds - 1.0);
            rep.pitchSemitones = -12.0 * std::log2 ((double) target / rate / rep.sourceSeconds);
            break;
        }
        case FitMode::Stretch:
        {
            auto st = stereoAtRate (src, rate);
            const double ratio = (double) target / (double) st.getNumSamples();
            out = runEngine (algo, st, ratio, pitch, rate, progress);
            rep.engineUsed = true;
            rep.stretchPercent = 100.0 * (ratio - 1.0);
            rep.pitchSemitones = pitch;
            break;
        }
        case FitMode::Loop:
        {
            auto st = stereoAtRate (src, rate);
            const int unit = loopUnitBars (rep.sourceBars, req.bars);
            juce::AudioBuffer<float> cell;
            juce::int64 cellLen;
            if (unit > 0)
            {
                // Lock one repeat to exactly `unit` bars first.
                cellLen = musical::samplesForBars (unit, req.tempo, rate);
                const double ratio = (double) cellLen / (double) st.getNumSamples();
                if (std::abs (ratio - 1.0) > 0.0005 || std::abs (pitch) > 1.0e-6)
                {
                    cell = runEngine (algo, st, ratio, pitch, rate, progress);
                    rep.engineUsed = true;
                    rep.stretchPercent = 100.0 * (ratio - 1.0);
                }
                else
                {
                    cell = trimTo (st, cellLen, rate);
                }
                rep.loopRepeats = req.bars / unit;
            }
            else
            {
                cell = std::abs (pitch) > 1.0e-6 ? runEngine (algo, st, 1.0, pitch, rate, progress) : st;
                rep.engineUsed = std::abs (pitch) > 1.0e-6;
                cellLen = cell.getNumSamples();
                rep.loopRepeats = (int) ((target + cellLen - 1) / juce::jmax<juce::int64> (1, cellLen));
            }
            rep.pitchSemitones = pitch;
            out.setSize (2, (int) target);
            out.clear();
            const int declick = juce::jmin ((int) (0.002 * rate), (int) cellLen / 4);
            for (int r = 0; ; ++r)
            {
                // Each repeat starts at its own rounded bar position: no cumulative drift.
                const juce::int64 start = unit > 0 ? musical::samplesForBars ((double) unit * r, req.tempo, rate) : (juce::int64) r * cellLen;
                if (start >= target)
                    break;
                const int n = (int) juce::jmin ((juce::int64) cell.getNumSamples(), target - start);
                for (int ch = 0; ch < 2; ++ch)
                    out.copyFrom (ch, (int) start, cell, juce::jmin (ch, cell.getNumChannels() - 1), 0, n);
                // Irregular loops: fade the end of each repeat (2 ms) so the seam doesn't click;
                // the next repeat's attack is left untouched.
                if (unit == 0 && start + n < target && declick > 0)
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        float* d = out.getWritePointer (ch, (int) start + n - declick);
                        for (int i = 0; i < declick; ++i)
                            d[i] *= 1.0f - (float) (i + 1) / (float) declick;
                    }
            }
            if (unit == 0 && cellLen < target && target % cellLen != 0)
                fadeOutTail (out, (int) target, (int) (0.004 * rate));
            break;
        }
        case FitMode::Trim:
        case FitMode::Auto:
        {
            auto st = stereoAtRate (src, rate);
            if (std::abs (pitch) > 1.0e-6)
            {
                st = runEngine (algo, st, 1.0, pitch, rate, progress);
                rep.engineUsed = true;
            }
            out = trimTo (st, target, rate);
            rep.pitchSemitones = pitch;
            break;
        }
    }

    if (progress.cancelled())
        return {};

    // Guarantee the contract: stereo, exactly targetSamples, finite.
    if (out.getNumChannels() != 2)
        out = assets::toStereo (out);
    if (out.getNumSamples() != target)
    {
        const int old = out.getNumSamples();
        out.setSize (2, (int) target, true, true, false);
        if (target > old)
            out.clear (old, (int) target - old);
    }
    for (int ch = 0; ch < 2; ++ch)
    {
        auto* d = out.getWritePointer (ch);
        for (int i = 0; i < out.getNumSamples(); ++i)
            if (! std::isfinite (d[i])) d[i] = 0.0f;
    }
    result.audio = std::make_shared<const juce::AudioBuffer<float>> (std::move (out));
    return result;
}
} // namespace timefit
} // namespace context
