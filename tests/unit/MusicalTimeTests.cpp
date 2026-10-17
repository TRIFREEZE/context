#include "TestSupport.h"
#include "Audio/MusicalTime.h"
#include "Audio/AudioAsset.h"
#include "Audio/Resampler.h"

namespace context::test
{
namespace
{
    /** Frequency estimate from positive-going zero crossings (good to ~0.1% for clean tones). */
    double zeroCrossingFrequency (const float* d, int n, double sr)
    {
        int first = -1, last = -1, count = 0;
        for (int i = 1; i < n; ++i)
            if (d[i - 1] < 0.0f && d[i] >= 0.0f)
            {
                if (first < 0) first = i;
                last = i;
                ++count;
            }
        return count > 1 ? (count - 1) * sr / (last - first) : 0.0;
    }

    juce::AudioBuffer<float> sine (double freq, double sr, int n, int channels = 1, float amp = 0.5f)
    {
        juce::AudioBuffer<float> b (channels, n);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < n; ++i)
                b.setSample (ch, i, amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / sr));
        return b;
    }

    double rms (const juce::AudioBuffer<float>& b, int ch, int start, int len)
    {
        double s = 0.0;
        for (int i = start; i < start + len; ++i)
            s += (double) b.getSample (ch, i) * b.getSample (ch, i);
        return std::sqrt (s / juce::jmax (1, len));
    }
}

class MusicalTimeTests final : public juce::UnitTest
{
public:
    MusicalTimeTests() : juce::UnitTest ("Musical time, assets and resampling", "V2") {}

    void runTest() override
    {
        using namespace musical;

        beginTest ("Bar durations");
        {
            const Tempo t120 { 120.0, { 4, 4 } };
            expectWithinAbsoluteError (secondsForBars (4, t120), 8.0, 1e-12);
            expectWithinAbsoluteError (secondsForBars (8, t120), 16.0, 1e-12);
            const Tempo t144 { 144.0, { 4, 4 } };
            expectWithinAbsoluteError (secondsForBars (8, t144), 13.333333333333334, 1e-9);   // 32 beats
            const Tempo sixEight { 120.0, { 6, 8 } };                                          // 3 quarter notes per bar
            expectWithinAbsoluteError (secondsForBars (4, sixEight), 6.0, 1e-12);
            const Tempo threeFour { 90.0, { 3, 4 } };
            expectWithinAbsoluteError (secondsForBars (2, threeFour), 4.0, 1e-12);
            expectEquals (secondsForBars (8, Tempo { 0.0, {} }), 0.0);
            expectEquals (secondsForBars (8, Tempo { 120.0, { 0, 4 } }), 0.0);
        }

        beginTest ("Exact sample counts at common sample rates");
        {
            const Tempo t120 { 120.0, { 4, 4 } };
            expectEquals (samplesForBars (4, t120, 44100.0), (juce::int64) 352800);
            expectEquals (samplesForBars (8, t120, 48000.0), (juce::int64) 768000);
            expectEquals (samplesForBars (8, t120, 96000.0), (juce::int64) 1536000);
            const Tempo t144 { 144.0, { 4, 4 } };
            expectEquals (samplesForBars (8, t144, 44100.0), (juce::int64) 588000);
            expectEquals (samplesForBars (8, t144, 48000.0), (juce::int64) 640000);
            expectEquals (samplesForBars (8, t144, 88200.0), (juce::int64) 1176000);
            expectEquals (samplesForBars (8, t144, 96000.0), (juce::int64) 1280000);
            // Non-integer case: 8 bars at 141 BPM / 44.1 kHz = 600510.638... -> rounds once.
            expectEquals (samplesForBars (8, Tempo { 141.0, {} }, 44100.0), (juce::int64) 600511);
            // 16 bars is exactly twice 8 bars whenever 8 bars is an integer (no drift).
            for (double bpm : { 90.0, 120.0, 144.0, 150.0 })
                for (double sr : { 44100.0, 48000.0, 96000.0 })
                {
                    const Tempo t { bpm, {} };
                    const auto eight = samplesForBars (8, t, sr);
                    if (std::abs (8 * 4 * 60.0 * sr / bpm - (double) eight) < 1e-6)
                        expectEquals (samplesForBars (16, t, sr), 2 * eight);
                }
            expectEquals (samplesForBars (2, t120, 0.0), (juce::int64) 0);
        }

        beginTest ("Next bar line from the play-head");
        {
            const TimeSignature fourFour {};
            expectWithinAbsoluteError (nextBarStartPpq (0.0, fourFour, std::nullopt), 0.0, 1e-12);
            expectWithinAbsoluteError (nextBarStartPpq (0.5, fourFour, std::nullopt), 4.0, 1e-12);
            expectWithinAbsoluteError (nextBarStartPpq (7.9999999, fourFour, std::nullopt), 8.0, 1e-9);
            expectWithinAbsoluteError (nextBarStartPpq (8.0000000001, fourFour, std::nullopt), 8.0, 1e-9);   // on the line
            expectWithinAbsoluteError (nextBarStartPpq (5.0, TimeSignature { 3, 4 }, std::nullopt), 6.0, 1e-12);
            expectWithinAbsoluteError (nextBarStartPpq (5.0, TimeSignature { 6, 8 }, std::nullopt), 6.0, 1e-12);
            // Host-supplied bar start (e.g. after a metre change at ppq 2)
            expectWithinAbsoluteError (nextBarStartPpq (3.0, fourFour, 2.0), 6.0, 1e-12);
            expectWithinAbsoluteError (nextBarStartPpq (2.0, fourFour, 2.0), 2.0, 1e-12);
        }

        beginTest ("Bars from durations and file-name tempo hints");
        {
            const Tempo t144 { 144.0, {} };
            expectWithinAbsoluteError (barsForSeconds (12.566667, t144), 7.54, 0.0001);
            expectWithinAbsoluteError (barsForSamples (640000, t144, 48000.0), 8.0, 1e-9);
            expectWithinAbsoluteError (bpmForLoop (16.0, 8.0, {}), 120.0, 1e-9);
            expect (bpmFromFileName ("guitar_loop_130bpm.wav") == 130.0);
            expect (bpmFromFileName ("Drums 92 BPM Dm.wav") == 92.0);
            expect (bpmFromFileName ("BPM_140 hats.wav") == 140.0);
            expect (! bpmFromFileName ("kick_808.wav").has_value());
            expect (! bpmFromFileName ("loop 9999bpm.wav").has_value());
        }

        beginTest ("Resampler keeps pitch and level, lengths exact");
        {
            const auto in = sine (1000.0, 44100.0, 44100);
            const auto out = dsp::resampler::convertRate (in, 44100.0, 48000.0);
            expectEquals (out.getNumSamples(), 48000);
            expectWithinAbsoluteError (zeroCrossingFrequency (out.getReadPointer (0), out.getNumSamples(), 48000.0), 1000.0, 0.5);
            expectWithinAbsoluteError (rms (out, 0, 2000, 40000), 0.5 / std::sqrt (2.0), 0.005);

            // Downsampling filters content above the new Nyquist instead of aliasing it.
            const auto high = sine (30000.0, 96000.0, 96000);
            const auto down = dsp::resampler::convertRate (high, 96000.0, 48000.0);
            expectEquals (down.getNumSamples(), 48000);
            expect (rms (down, 0, 2000, 40000) < 0.5 * 0.001, "30 kHz must be removed when going to 48 kHz: " + juce::String (rms (down, 0, 2000, 40000)));

            // Tape-style length change moves pitch by the length ratio.
            const auto tape = dsp::resampler::toLength (in, 22050);
            expectEquals (tape.getNumSamples(), 22050);
            expectWithinAbsoluteError (zeroCrossingFrequency (tape.getReadPointer (0), 22050, 44100.0), 2000.0, 1.0);
        }

        beginTest ("Assets: write, load, sanitise, stereo, peaks");
        {
            auto dir = scratchDir ("assets");
            auto stereoIn = sine (220.0, 48000.0, 4800, 2);
            stereoIn.setSample (1, 10, std::numeric_limits<float>::quiet_NaN());
            juce::String err;
            const auto f = dir.getChildFile ("tone.wav");
            expect (assets::writeWav (stereoIn, 48000.0, f, 24, err), err);
            expect (! dir.getChildFile ("tone.writing.tmp").exists());
            auto loaded = assets::loadFile (f, AudioAsset::Origin::Imported);
            expect (loaded.ok(), loaded.error);
            if (loaded.ok())
            {
                expectEquals (loaded.asset->numChannels(), 2);
                expectEquals ((int) loaded.asset->numSamples(), 4800);
                expectEquals (loaded.asset->sampleRate, 48000.0);
                expect (std::isfinite (loaded.asset->audio.getSample (1, 10)));
                expect (loaded.asset->origin == AudioAsset::Origin::Imported);
            }

            const auto mono = sine (220.0, 44100.0, 100, 1);
            const auto st = assets::toStereo (mono);
            expectEquals (st.getNumChannels(), 2);
            expectEquals (st.getSample (1, 50), mono.getSample (0, 50));   // equal level, no +3 dB

            auto bad = dir.getChildFile ("bad.wav");
            bad.replaceWithText ("not audio");
            expect (! assets::loadFile (bad, AudioAsset::Origin::Imported).ok());
            auto txt = dir.getChildFile ("notes.txt");
            txt.replaceWithText ("hello");
            const auto txtResult = assets::loadFile (txt, AudioAsset::Origin::Imported);
            expect (! txtResult.ok() && txtResult.error.contains ("isn't supported"));
            expect (! assets::loadFile (dir.getChildFile ("missing.wav"), AudioAsset::Origin::Imported).ok());

            const auto peaks = WaveformPeaks::build (stereoIn, 48000.0, 64);
            expectEquals (peaks->numPeaks(), (4800 + 63) / 64);
            const auto r = peaks->range (0, 4800);
            expect (r.first < -0.45f && r.second > 0.45f);
        }
    }
};

static MusicalTimeTests musicalTimeTests;
} // namespace context::test
