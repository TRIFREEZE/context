#include "PromptBuilder.h"
#include <regex>

namespace context
{
namespace
{
    bool containsWord (const juce::String& text, std::initializer_list<const char*> words)
    {
        for (auto* w : words)
        {
            const juce::String word (w);
            int idx = text.indexOf (word);
            while (idx >= 0)
            {
                const bool startOk = idx == 0 || ! juce::CharacterFunctions::isLetterOrDigit (text[idx - 1]);
                const int end = idx + word.length();
                // allow simple plurals / suffixes ("kicks", "808s", "chopped")
                const bool endOk = end >= text.length() || ! juce::CharacterFunctions::isLetter (text[end])
                                || text.substring (end, end + 1) == "s" || text.substring (end, end + 2) == "ed"
                                || text.substring (end, end + 3) == "ped" || text.substring (end, end + 3) == "ing";
                if (startOk && endOk)
                    return true;
                idx = text.indexOf (idx + 1, word);
            }
        }
        return false;
    }

    juce::String capitaliseFirst (juce::String s)
    {
        s = s.trim();
        if (s.isEmpty())
            return s;
        return s.substring (0, 1).toUpperCase() + s.substring (1);
    }

    juce::String sentence (juce::String s)
    {
        s = s.trim();
        if (s.isNotEmpty() && ! s.endsWithChar ('.') && ! s.endsWithChar ('!') && ! s.endsWithChar ('?'))
            s << ".";
        return s;
    }

    juce::String bpmText (double bpm)
    {
        const double rounded = std::round (bpm * 10.0) / 10.0;
        return std::abs (rounded - std::round (rounded)) < 0.05 ? juce::String ((int) std::round (rounded)) : juce::String (rounded, 1);
    }

    bool isTonalType (GenerationType t)
    {
        return t == GenerationType::Vocal || t == GenerationType::Instrument || t == GenerationType::Melody
            || t == GenerationType::Texture || t == GenerationType::Loop;
    }
}

PromptBuilder::Intent PromptBuilder::detectIntent (const juce::String& promptIn)
{
    Intent i;
    const auto p = promptIn.toLowerCase();

    try
    {
        const std::string s = p.toStdString();
        static const std::regex bpmRe (R"((\d{2,3}(?:\.\d+)?)\s*-?\s*bpm)");
        std::smatch m;
        if (std::regex_search (s, m, bpmRe))
        {
            const double v = std::stod (m[1].str());
            if (v >= 40.0 && v <= 300.0)
                i.explicitBpm = v;
        }
        // "F#", "Bb minor", "C# maj"  (an accidental makes a bare letter unambiguous)
        static const std::regex keyAccidental (R"((^|[^a-z])[a-g](#|b|\s?sharp|\s?flat)\s?(m|maj|major|min|minor)?([^a-z0-9]|$))");
        // "A minor", "C major", "cmaj"
        static const std::regex keyNamed (R"((^|[^a-z])[a-g]\s?(maj|major|min|minor)([^a-z]|$))");
        i.explicitKey = std::regex_search (s, keyAccidental) || std::regex_search (s, keyNamed);
    }
    catch (...) {}

    i.wantsLowEnd    = containsWord (p, { "sub", "808", "bass", "low end", "low-end", "rumble", "boomy", "subby" });
    i.wantsBright    = containsWord (p, { "bright", "airy", "crisp", "sparkly", "shimmer", "shiny" });
    i.wantsDark      = containsWord (p, { "dark", "muffled", "lofi", "lo-fi", "warm", "murky", "gloomy" });
    i.wantsWide      = containsWord (p, { "wide", "stereo", "spacious", "panned" });
    i.wantsMono      = containsWord (p, { "mono", "centered", "centred", "narrow" });
    i.wantsShort     = containsWord (p, { "short", "chop", "stab", "hit", "one shot", "one-shot", "staccato", "stutter" });
    i.wantsSustained = containsWord (p, { "pad", "drone", "ambient", "sustained", "long", "swell", "evolving" });
    i.drumSubject    = containsWord (p, { "kick", "snare", "hat", "hihat", "hi-hat", "clap", "perc", "percussion", "drum",
                                          "drums", "rim", "tom", "shaker", "cymbal", "cowbell", "808" });
    i.tonalDrum      = containsWord (p, { "808", "tuned", "cowbell", "tom", "bass" });
    return i;
}

PromptBuilder::Output PromptBuilder::build (const Inputs& in)
{
    Output out;
    const auto prompt = in.userPrompt.trim();
    const auto intent = detectIntent (prompt);
    const auto& a = in.analysis;

    // ---- Structured fields (sent even when text expansion is off) ----
    if (intent.explicitBpm)
        out.bpm = intent.explicitBpm;
    else if (in.hostBpm)
        out.bpm = in.hostBpm;
    else if (a && a->valid && a->estimatedBpm && a->tempoConfidence > 0.3)
        out.bpm = a->estimatedBpm;

    const bool keyUseful = isTonalType (in.type) && ! (intent.drumSubject && ! intent.tonalDrum);
    const bool keyUsefulForDrum = intent.tonalDrum;
    if (in.userKey && in.userKey->first >= 0 && in.userKey->first < 12 && ! intent.explicitKey && (keyUseful || keyUsefulForDrum))
    {
        out.key = pitchClassName (in.userKey->first);
        out.mode = in.userKey->second ? "minor" : "major";
        out.keyConfidence = 1.0;
    }
    else if (a && a->valid && a->keyKnown && ! intent.explicitKey && (keyUseful || keyUsefulForDrum))
    {
        out.key = a->keyName();
        out.mode = a->modeName().toLowerCase();
        out.keyConfidence = a->keyConfidence;
    }

    // Things to avoid, from your passes: structured, so they go out even with Enhance off.
    out.negativeTags = in.avoid.joinIntoString (", ").substring (0, 200);

    if (! in.expansionEnabled || prompt.isEmpty())
    {
        out.expandedPrompt = prompt;
        return out;
    }

    juce::StringArray parts;
    parts.add (sentence (capitaliseFirst (prompt)));

    // ---- Generation type ----
    const auto lower = prompt.toLowerCase();
    switch (in.type)
    {
        case GenerationType::OneShot:
            if (! containsWord (lower, { "one shot", "one-shot", "hit", "shot", "stab" }))
                { parts.add ("Single one-shot sample with a clean start and natural decay."); out.hints.add ("one-shot"); }
            break;
        case GenerationType::Loop:
            if (! containsWord (lower, { "loop" }))
                { parts.add ("Seamless, loopable phrase."); out.hints.add ("loop"); }
            break;
        case GenerationType::Percussion:
            if (! intent.drumSubject)
                { parts.add ("Percussive sound."); out.hints.add ("percussion"); }
            break;
        case GenerationType::Vocal:
            if (! containsWord (lower, { "vocal", "vox", "voice", "choir", "sing", "singer", "rap", "chant" }))
                { parts.add ("Vocal sound."); out.hints.add ("vocal"); }
            break;
        case GenerationType::Texture:
            if (! containsWord (lower, { "texture", "pad", "ambient", "atmosphere", "drone" }))
                { parts.add ("Atmospheric texture."); out.hints.add ("texture"); }
            break;
        case GenerationType::FX:
            if (! containsWord (lower, { "fx", "riser", "impact", "sweep", "whoosh", "transition", "effect" }))
                { parts.add ("Sound effect / transition element."); out.hints.add ("fx"); }
            break;
        case GenerationType::Melody:
            if (! containsWord (lower, { "melody", "melodic", "riff", "hook", "lead" }))
                { parts.add ("Melodic phrase."); out.hints.add ("melody"); }
            break;
        case GenerationType::Instrument:
            break;
    }

    // ---- Tempo + key ----
    juce::String tempoKey;
    if (out.bpm && ! intent.explicitBpm)
        tempoKey << bpmText (*out.bpm) << " BPM track";
    if (out.key.isNotEmpty())
        tempoKey << (tempoKey.isEmpty() ? "track " : " ") << "in " << out.key << " " << out.mode;

    if (tempoKey.isNotEmpty())
    {
        const bool lowConfidence = out.keyConfidence && *out.keyConfidence < 0.65;
        if (in.type == GenerationType::Loop || in.type == GenerationType::Percussion || in.type == GenerationType::Melody)
            parts.add ("Designed for a " + tempoKey + ", locked to the grid.");
        else
            parts.add ("Designed for a " + tempoKey + ".");
        if (lowConfidence)
            parts.add ("(Key detection is uncertain; stay harmonically simple.)");
        if (out.bpm && ! intent.explicitBpm) out.hints.add (bpmText (*out.bpm) + " BPM");
        if (out.key.isNotEmpty())            out.hints.add (out.key + " " + out.mode);
    }

    if (in.type == GenerationType::Vocal && containsWord (lower, { "chop", "chops", "stutter" }) && out.bpm)
        { parts.add ("Short rhythmic phrasing."); out.hints.add ("rhythmic phrasing"); }

    // ---- Your verdicts on this prompt so far ----
    if (! in.steerHints.isEmpty())
    {
        parts.add (sentence (capitaliseFirst (in.steerHints.joinIntoString ("; "))));
        out.hints.add ("steered by your passes");
    }

    if (! a || ! a->valid)
    {
        out.expandedPrompt = parts.joinIntoString (" ").substring (0, maxLength);
        return out;
    }

    // ---- Frequency balance (user intent outranks heuristics) ----
    const bool bassLike = intent.wantsLowEnd || (intent.drumSubject && containsWord (lower, { "kick", "808", "tom" }));
    if (a->lowEnd == Level::High && ! bassLike)
    {
        juce::String s = "The current production already has strong low-frequency energy, so keep unnecessary sub-bass minimal";
        if (in.type == GenerationType::Vocal)            s << " while preserving vocal body";
        else if (in.type == GenerationType::Instrument
              || in.type == GenerationType::Melody)      s << " and let the body sit in the low mids";
        parts.add (s + ".");
        out.hints.add ("less sub-bass");
    }
    else if (a->lowEnd == Level::Low && bassLike)
    {
        parts.add ("The track has room in the low end, so a full, weighty bottom is welcome.");
        out.hints.add ("room in low end");
    }

    if (a->mids == Level::High && (in.type == GenerationType::Vocal || in.type == GenerationType::Instrument || in.type == GenerationType::Melody))
    {
        parts.add ("The mids are already busy, so aim for a focused tone that cuts through without crowding them.");
        out.hints.add ("focused mids");
    }

    if (a->highs == Level::Low && ! intent.wantsDark)
    {
        parts.add ("The track's top end is sparse, so crisp high-frequency detail is welcome.");
        out.hints.add ("add top end");
    }
    else if (a->highs == Level::High && ! intent.wantsBright && in.type != GenerationType::Percussion && ! intent.drumSubject)
    {
        parts.add ("The top end is already bright, so keep harsh highs under control.");
        out.hints.add ("tame highs");
    }

    if (! intent.wantsDark && ! intent.wantsBright)
    {
        if (a->tonal == TonalBalance::Dark)        { parts.add ("Match the track's dark tonal character."); out.hints.add ("dark tone"); }
        else if (a->tonal == TonalBalance::Bright) { parts.add ("Match the track's bright tonal character."); out.hints.add ("bright tone"); }
    }

    // ---- Stereo ----
    if (bassLike && ! intent.wantsWide && a->stereo != StereoWidth::Narrow)
    {
        parts.add ("Keep it mono-compatible and centered.");
        out.hints.add ("mono-compatible");
    }
    else if (a->stereo == StereoWidth::Narrow && ! intent.wantsMono && ! bassLike
             && (in.type == GenerationType::Texture || in.type == GenerationType::FX || in.type == GenerationType::Instrument || in.type == GenerationType::Vocal))
    {
        parts.add ("The mix is fairly narrow, so a wide stereo image will add space.");
        out.hints.add ("wide stereo");
    }

    // ---- Transients / density ----
    if (a->transients == TransientDensity::Dense && ! intent.wantsShort
        && (in.type == GenerationType::Vocal || in.type == GenerationType::Texture || in.type == GenerationType::Instrument || in.type == GenerationType::Melody))
    {
        parts.add ("The groove is already busy, so favor smooth, sustained phrasing.");
        out.hints.add ("smooth phrasing");
    }
    else if (a->transients == TransientDensity::Sparse && ! intent.wantsSustained
             && (in.type == GenerationType::Percussion || in.type == GenerationType::Loop || intent.drumSubject))
    {
        parts.add ("The arrangement is sparse, so punchy, well-defined transients will help.");
        out.hints.add ("punchy transients");
    }

    out.expandedPrompt = parts.joinIntoString (" ").substring (0, maxLength);
    return out;
}
} // namespace context
