#pragma once

#include "Audio/MusicalTime.h"
#include "Audio/TimeStretch/TimeStretchEngine.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <map>
#include <memory>
#include <vector>

namespace context::edit
{
/** One version of the output sound: a generation (by history id) or a sample file.
    Every take is fitted to the same N bars at the same tempo, so the same bar of two takes
    lines up musically and pieces can be swapped between them ("comping"). */
struct Take
{
    enum class Kind { Generated, Imported };
    juce::String id;            // short id unique within the session ("t3")
    Kind kind = Kind::Generated;
    juce::String ref;           // generation id, or absolute file path
    juce::String name;          // prompt / file name, for the UI

    bool operator== (const Take&) const = default;
    juce::var toVar() const;
    static std::optional<Take> fromVar (const juce::var&);
};

/** A piece of a take placed on the output timeline. All positions are in QUARTER NOTES
    (beats), so edits stay on the musical grid when the tempo or sample rate changes. */
struct Clip
{
    juce::String take;
    double start = 0.0;         // timeline position
    double length = 0.0;        // timeline length
    double offset = 0.0;        // where in the take it starts
    double stretch = 1.0;       // take beats played per timeline beat (1 = untouched)
    float gainDb = 0.0f;

    double end() const noexcept          { return start + length; }
    double sourceLength() const noexcept { return length * stretch; }
    bool operator== (const Clip&) const = default;
};

/** The output lane's arrangement. Clips are sorted, never overlap, and gaps are silence.
    `active == false` means "no edits": the base take plays from start to end, exactly as
    before editing existed (old projects load like that). An active edit with no clips is a
    lane the user deleted everything from. */
struct Edit
{
    bool active = false;
    std::vector<Clip> clips;

    bool isEdited() const noexcept { return active; }
    bool operator== (const Edit&) const = default;
    juce::var toVar() const;   // void when not edited
    /** Tolerant: drops clips with unknown takes, bad numbers or overlaps. */
    static Edit fromVar (const juce::var&, const juce::StringArray& validTakeIds);
};

inline constexpr double minClipBeats = 1.0 / 64.0;
inline constexpr double maxStretch = 4.0;
inline constexpr double minStretch = 0.25;

// ---- pure operations (message thread; all unit-tested) ---------------------------------
// `total` is the timeline length in beats. Every function returns a valid, normalised Edit.

/** The explicit clip list (an unedited lane becomes one clip of the base take). */
Edit materialise (const Edit&, const juce::String& baseTake, double total);
/** Back to "no edits" when the clips describe exactly the unedited base take; merges
    neighbours that continue each other seamlessly. */
Edit simplify (const Edit&, const juce::String& baseTake, double total);

Edit removeRange (const Edit&, const juce::String& baseTake, double total, double from, double to);
Edit place (const Edit&, const juce::String& baseTake, double total, Clip);
Edit splitAt (const Edit&, const juce::String& baseTake, double total, double beat);
/** Comp: the same bars of another take replace [from, to). */
Edit fillFromTake (const Edit&, const juce::String& baseTake, double total, double from, double to, const juce::String& take);
Edit moveClip (const Edit&, const juce::String& baseTake, double total, int index, double newStart);
/** Drag a clip edge: the start edge moves start+offset together (audio stays in place). */
Edit trimClip (const Edit&, const juce::String& baseTake, double total, int index, bool startEdge, double newEdge);
/** Drag the end edge with stretch: the clip's audio is time-stretched to the new length. */
Edit stretchClip (const Edit&, const juce::String& baseTake, double total, int index, double newEnd);
/** Drag the start edge with stretch: the end stays put, the audio is stretched to fit. */
Edit stretchClipStart (const Edit&, const juce::String& baseTake, double total, int index, double newStart);
Edit setClipGain (const Edit&, const juce::String& baseTake, double total, int index, float gainDb);
/** Clips in [from, to), relative to `from` (a clipboard). */
std::vector<Clip> copyRange (const Edit&, const juce::String& baseTake, double total, double from, double to);
Edit paste (const Edit&, const juce::String& baseTake, double total, const std::vector<Clip>&, double at);

/** Index of the clip under `beat` in a materialised edit, or -1. */
int clipAt (const Edit&, const juce::String& baseTake, double total, double beat);

/** Snap to a grid of `division` beats (0 = off). */
double snap (double beat, double division);

// ---- rendering (worker thread) ---------------------------------------------------------

using TakeAudio = std::map<juce::String, std::shared_ptr<const juce::AudioBuffer<float>>>;

/** Composes the arrangement into exactly `totalSamples` of stereo audio at `rate`.
    Clip boundaries get 2.5 ms crossfades (seamless continuations get none), stretched clips
    go through the time-stretch engine. Missing takes render as silence. */
juce::AudioBuffer<float> render (const Edit&, const juce::String& baseTake, const TakeAudio&,
                                 const musical::Tempo&, double rate, juce::int64 totalSamples,
                                 stretch::Algorithm algorithm = stretch::Algorithm::Auto);

juce::int64 samplesForBeats (double beats, const musical::Tempo&, double rate);
} // namespace context::edit
