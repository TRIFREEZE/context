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
    o->setProperty ("tighten", tighten);
    return juce::var (o);
}

FitSettings FitSettings::fromVar (const juce::var& v)
{
    FitSettings s;
    s.mode = fitModeFromId (v.getProperty ("mode", {}).toString()).value_or (FitMode::Auto);
    s.algorithm = stretch::algorithmFromId (v.getProperty ("algorithm", {}).toString()).value_or (stretch::Algorithm::Auto);
    const double p = (double) v.getProperty ("pitch", 0.0);
    s.pitchSemitones = std::isfinite (p) ? juce::jlimit (-24.0, 24.0, p) : 0.0;
    s.tighten = (bool) v.getProperty ("tighten", true);
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

    /** Copy (or pad) to exactly `length` from `offset`, fading out if the cut lands on sound. */
    juce::AudioBuffer<float> trimTo (const juce::AudioBuffer<float>& in, juce::int64 length, double rate, juce::int64 offset = 0)
    {
        juce::AudioBuffer<float> out (2, (int) length);
        out.clear();
        offset = juce::jlimit<juce::int64> (0, in.getNumSamples(), offset);
        const int n = (int) juce::jmin<juce::int64> (length, in.getNumSamples() - offset);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, 0, in, juce::jmin (ch, in.getNumChannels() - 1), (int) offset, n);
        if (offset > 0 && n > 0)
        {
            const int fade = juce::jmin (n / 4, (int) (0.003 * rate));
            for (int ch = 0; ch < 2; ++ch)
                out.applyGainRamp (ch, 0, fade, 0.0f, 1.0f);
        }
        if (in.getNumSamples() - offset > length && n > 0)
            if (std::abs (out.getSample (0, n - 1)) > 1.0e-3f || std::abs (out.getSample (1, n - 1)) > 1.0e-3f)
                fadeOutTail (out, n, (int) (0.004 * rate));
        return out;
    }

    /** For a context-aligned result whose sound starts late: which whole bar to start from.

        Kie.ai (and other bridges) can return a part whose sound only starts after a long gap, or
        a song longer than the context whose part comes in later; keeping the first bars would
        give silence. Find where the sound starts (the first moment within 30 dB of the peak) and
        move back to a bar line in a musically safe step: a whole `bars`-long loop if that leaves
        at most one bar of lead-in, else 4, 2 or 1 bar(s). Only when more than half the loop would
        otherwise be silent: a part that comes in at bar 3 is left where it is. */
    int alignedStartBar (const juce::AudioBuffer<float>& st, double rate, const musical::Tempo& tempo, int bars)
    {
        const double bar = (double) musical::samplesForBars (1, tempo, rate);
        const auto total = (juce::int64) st.getNumSamples();
        if (bar <= 0.0 || total <= 0)
            return 0;
        const int hop = juce::jmax (1, (int) (0.02 * rate));
        std::vector<float> env;
        float peak = 0.0f;
        for (juce::int64 i = 0; i + hop <= total; i += hop)
        {
            float m = 0.0f;
            for (int ch = 0; ch < st.getNumChannels(); ++ch)
            {
                const float* d = st.getReadPointer (ch, (int) i);
                for (int k = 0; k < hop; k += 2)
                    m = juce::jmax (m, std::abs (d[k]));
            }
            env.push_back (m);
            peak = juce::jmax (peak, m);
        }
        if (peak <= 1.0e-5f)
            return 0;
        const float threshold = peak * 0.0316f;           // -30 dB re peak
        size_t first = 0;
        while (first < env.size() && env[first] < threshold)
            ++first;
        const double onsetBars = (double) first * hop / bar;
        if (onsetBars <= bars * 0.5)
            return 0;                                      // a part coming in mid-loop is musical: leave it
        for (int step : { bars, 4, 2, 1 })
        {
            if (step < 1 || step > bars)
                continue;
            const int start = (int) std::floor (onsetBars / step) * step;
            if (onsetBars - start <= 1.0 + 1.0e-6)
                return start;
        }
        return (int) std::floor (onsetBars);
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

ContentRange detectContent (const juce::AudioBuffer<float>& b, double rate)
{
    const juce::int64 n = b.getNumSamples();
    ContentRange r { 0, n };
    if (n == 0 || rate <= 0.0)
        return r;
    float peak = 0.0f;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        peak = juce::jmax (peak, b.getMagnitude (ch, 0, (int) n));
    if (peak < 1.0e-4f)
        return r;                                               // silence: nothing to tighten
    const float headThreshold = juce::jmax (peak * 0.005623f, 1.0e-4f);   // -45 dB re peak
    const float tailThreshold = juce::jmax (peak * 0.001f, 3.0e-5f);      // -60 dB re peak (keeps tails)
    auto loud = [&] (juce::int64 i, float t)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            if (std::abs (b.getSample (ch, (int) i)) > t)
                return true;
        return false;
    };
    juce::int64 first = 0;
    while (first < n && ! loud (first, headThreshold))
        ++first;
    juce::int64 last = n - 1;
    while (last > first && ! loud (last, tailThreshold))
        --last;

    const auto preRoll = (juce::int64) (0.002 * rate);
    const auto minHead = (juce::int64) (0.010 * rate);         // don't nibble a few ms off tight files
    if (first > minHead)
        r.start = juce::jmax<juce::int64> (0, first - preRoll);
    // Only real dead air at the end (> 0.5 s) is removed: shorter rests are part of the rhythm.
    const auto tailPad = (juce::int64) (0.030 * rate);
    if (n - 1 - last > (juce::int64) (0.5 * rate))
        r.end = juce::jmin (n, last + 1 + tailPad);
    if (r.end - r.start < (juce::int64) (0.010 * rate))
        return { 0, n };
    return r;
}

AudioAsset::Ptr tighten (const AudioAsset::Ptr& src, const musical::Tempo& project, double& trimmedStart, double& trimmedEnd)
{
    trimmedStart = trimmedEnd = 0.0;
    // Only generated audio is tightened: a sample the user dropped in is trusted to start where
    // they want it to (a loop may begin with a rest on beat 1).
    if (src == nullptr || ! src->isValid() || src->alignedToContext || src->origin != AudioAsset::Origin::Generated)
        return src;
    // A designed loop that already lasts a whole number of bars keeps its silences: they are
    // part of the groove (a rest on beat 1 is not dead space).
    if (src->designedLoop)
    {
        juce::String origin;
        const auto bpm = inferSourceBpm (*src, project, origin);
        const musical::Tempo t { bpm.value_or (project.bpm), project.signature };
        const double bars = musical::barsForSeconds (src->seconds(), t);
        if (bars >= 0.9 && std::abs (bars - std::round (bars)) <= 0.02 * std::round (bars))
            return src;
    }
    const auto range = detectContent (src->audio, src->sampleRate);
    if (range.start == 0 && range.end == src->numSamples())
        return src;

    auto out = std::make_shared<AudioAsset> (*src);
    const int len = (int) (range.end - range.start);
    out->audio.setSize (src->numChannels(), len);
    for (int ch = 0; ch < src->numChannels(); ++ch)
        out->audio.copyFrom (ch, 0, src->audio, ch, (int) range.start, len);
    // 1 ms fade-in where we cut into the (quiet) pre-roll, so the cut can never click.
    if (range.start > 0)
    {
        const int fade = juce::jmin (len, (int) (0.001 * src->sampleRate));
        for (int ch = 0; ch < out->numChannels(); ++ch)
            out->audio.applyGainRamp (ch, 0, fade, 0.0f, 1.0f);
    }
    if (range.end < src->numSamples())
    {
        const int fade = juce::jmin (len, (int) (0.010 * src->sampleRate));
        for (int ch = 0; ch < out->numChannels(); ++ch)
            out->audio.applyGainRamp (ch, len - fade, fade, 1.0f, 0.0f);
    }
    trimmedStart = range.start / src->sampleRate;
    trimmedEnd = (src->numSamples() - range.end) / src->sampleRate;
    return out;
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

    auto& rep = result.report;
    auto working = req.source;
    if (req.settings.tighten)
        working = tighten (req.source, req.tempo, rep.trimmedStartSeconds, rep.trimmedEndSeconds);
    // Made on the context but its sound starts late: start from the right bar line, in every
    // mode (shorter what's left? Auto loops it in whole bars below).
    if (working->alignedToContext && working->isValid())
    {
        const int startBar = alignedStartBar (working->audio, working->sampleRate, req.tempo, req.bars);
        const auto off = musical::samplesForBars (startBar, req.tempo, working->sampleRate);
        if (startBar > 0 && off < working->audio.getNumSamples())
        {
            auto moved = std::make_shared<AudioAsset> (*working);
            const int n = working->audio.getNumSamples() - (int) off;
            moved->audio.setSize (working->audio.getNumChannels(), n);
            for (int ch = 0; ch < working->audio.getNumChannels(); ++ch)
            {
                moved->audio.copyFrom (ch, 0, working->audio, ch, (int) off, n);
                moved->audio.applyGainRamp (ch, 0, juce::jmin (n, (int) (0.003 * working->sampleRate)), 0.0f, 1.0f);
            }
            // Shorter than the loop now: keep whole bars, so it can repeat on the grid.
            const double leftBars = n / (double) musical::samplesForBars (1, req.tempo, working->sampleRate);
            if (leftBars < req.bars && leftBars >= 1.0)
                moved->audio.setSize (moved->audio.getNumChannels(),
                                      (int) musical::samplesForBars (std::floor (leftBars + 1.0e-6), req.tempo, working->sampleRate), true);
            working = moved;
            rep.alignedStartBar = startBar;
        }
    }
    const auto& src = *working;
    rep.aligned = src.alignedToContext;
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
    // A moved part shorter than the loop repeats in whole bars in every mode (stretching one bar
    // over eight would be nothing like what was generated).
    if (rep.alignedStartBar > 0 && rep.sourceBars + 0.05 < req.bars && loopUnitBars (rep.sourceBars, req.bars) > 0)
        mode = FitMode::Loop;
    else if (rep.aligned && req.settings.mode == FitMode::Auto)
    {
        // Generated on the context at the context's tempo: keep every sample where it is.
        const int unit = loopUnitBars (rep.sourceBars, req.bars);
        mode = (rep.sourceBars + 0.05 < req.bars && unit > 0 && unit < req.bars) ? FitMode::Loop : FitMode::Trim;
    }
    rep.modeUsed = mode;

    auto algo = req.settings.algorithm;
    if (algo == stretch::Algorithm::Auto)
        algo = (req.material && working == req.source ? *req.material : stretch::analyseMaterial (src.audio, src.sampleRate)).suggested;
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
                if (rep.aligned)
                {
                    // Context-aligned: never stretch, just cut each repeat to exactly `unit` bars.
                    cell = trimTo (std::abs (pitch) > 1.0e-6 ? runEngine (algo, st, 1.0, pitch, rate, progress) : st, cellLen, rate);
                    rep.engineUsed = std::abs (pitch) > 1.0e-6;
                }
                else if (std::abs (ratio - 1.0) > 0.0005 || std::abs (pitch) > 1.0e-6)
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
