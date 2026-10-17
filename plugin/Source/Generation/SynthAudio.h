#pragma once

#include "GenerationModels.h"
#include <juce_audio_basics/juce_audio_basics.h>

namespace context::synth
{
/** Programmatically generated test audio for the Mock provider.
    No samples, no copyrighted material: everything is built from oscillators,
    noise and simple filters. The prompt's keywords pick the sound
    (kick, snare, hat, clap, 808/bass, cowbell, pad/texture, vocal, guitar,
    riser/impact FX, melody, percussion loop) and modifiers (dark, bright,
    distorted, wide, reverse, short, long). */
struct Rendered
{
    juce::AudioBuffer<float> audio;
    double sampleRate = 44100.0;
    juce::String description;     // which recipe was used, for metadata/logging
};

Rendered render (const GenerationRequest& request, juce::int64 seed, double sampleRate = 44100.0);

/** Writes a 24-bit stereo WAV. Returns false on any I/O failure. */
bool writeWav (const juce::AudioBuffer<float>& audio, double sampleRate, const juce::File& file);

/** MIDI note number for a pitch class in a given octave (C4 = 60). */
int noteNumber (int pitchClass, int octave);
double noteToHz (double midiNote);
} // namespace context::synth
