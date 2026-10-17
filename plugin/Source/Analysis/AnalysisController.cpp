#include "AnalysisController.h"
#include "Generation/SynthAudio.h"
#include "Storage/Logger.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace context
{
AnalysisController::AnalysisController (CaptureBuffer& c, std::function<std::optional<double>()> bpm)
    : capture (c), hostBpm (std::move (bpm))
{
    worker.onTaskException = [] (const juce::String& what) { logging::error ("analysis", "Analysis threw: " + what); };
}

AnalysisController::~AnalysisController()
{
    worker.clearPending();
}

bool AnalysisController::requestAnalysis()
{
    {
        const juce::ScopedLock sl (lock);
        if (state == State::Analyzing)
            return false;
        state = State::Analyzing;
        message = "Analyzing...";
    }
    if (! worker.post ([this] { run(); }))
    {
        const juce::ScopedLock sl (lock);
        state = State::Failed;
        message = "Analysis is busy. Try again.";
        return false;
    }
    sendChangeMessage();
    return true;
}

void AnalysisController::run()
{
    const auto snap = capture.snapshot();
    const auto bpm = hostBpm ? hostBpm() : std::nullopt;

    if (snap.seconds() < analysis::minimumSeconds)
    {
        {
            const juce::ScopedLock sl (lock);
            state = State::NotEnoughAudio;
            message = "Not enough audio has been captured for analysis. Play your track through Context for a few seconds, then try again.";
        }
        logging::info ("analysis", "Not enough audio (" + juce::String (snap.seconds(), 1) + " s)");
        sendChangeMessage();
        return;
    }

    const auto start = juce::Time::getMillisecondCounterHiRes();
    auto result = analysis::analyze (snap.audio, snap.sampleRate, bpm);
    const auto ms = juce::Time::getMillisecondCounterHiRes() - start;

    {
        const juce::ScopedLock sl (lock);
        if (! result.valid)
        {
            state = State::NotEnoughAudio;
            message = "The captured audio is too quiet to analyze.";
        }
        else
        {
            state = State::Done;
            message = {};
            latest = result;
        }
    }
    logging::info ("analysis", juce::String::formatted ("Analyzed %.1f s in %.0f ms: ", snap.seconds(), ms)
                               + result.keyDisplay() + juce::String::formatted (" (%.0f%%), ", result.keyConfidence * 100.0)
                               + "low " + toString (result.lowEnd) + ", stereo " + toString (result.stereo)
                               + ", transients " + toString (result.transients));
    sendChangeMessage();
}

AnalysisController::State AnalysisController::getState() const   { const juce::ScopedLock sl (lock); return state; }
juce::String AnalysisController::getMessage() const               { const juce::ScopedLock sl (lock); return message; }
std::optional<TrackAnalysis> AnalysisController::getLatest() const { const juce::ScopedLock sl (lock); return latest; }

void AnalysisController::clear()
{
    {
        const juce::ScopedLock sl (lock);
        if (state != State::Analyzing)
            state = State::Idle;
        latest.reset();
        message = {};
    }
    sendChangeMessage();
}

void AnalysisController::setExternalResult (const TrackAnalysis& result)
{
    {
        const juce::ScopedLock sl (lock);
        if (state != State::Analyzing)
            state = State::Done;
        message = {};
        latest = result;
    }
    sendChangeMessage();
}

std::optional<AudioConditioningPayload> AnalysisController::writeAudioContext (const juce::File& file, double seconds) const
{
    const auto snap = capture.snapshot();
    if (snap.seconds() < 1.0 || snap.sampleRate <= 0)
        return std::nullopt;

    const int take = juce::jmin (snap.audio.getNumSamples(), (int) (seconds * snap.sampleRate));
    const int offset = snap.audio.getNumSamples() - take;
    std::vector<float> mono ((size_t) take);
    for (int i = 0; i < take; ++i)
        mono[(size_t) i] = 0.5f * (snap.audio.getSample (0, offset + i) + snap.audio.getSample (1, offset + i));

    constexpr double targetRate = 22050.0;
    const double ratio = snap.sampleRate / targetRate;
    const int outLen = juce::jmax (1, (int) (take / ratio));
    juce::AudioBuffer<float> out (1, outLen);
    juce::LagrangeInterpolator interp;
    interp.process (ratio, mono.data(), out.getWritePointer (0), outLen);

    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    if (! static_cast<juce::FileOutputStream*> (stream.get())->openedOk())
        return std::nullopt;
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (targetRate).withNumChannels (1).withBitsPerSample (16));
    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (out, 0, outLen))
    {
        writer.reset();
        file.deleteFile();
        return std::nullopt;
    }
    writer.reset();

    AudioConditioningPayload p;
    p.wavFile = file;
    p.seconds = outLen / targetRate;
    p.sampleRate = targetRate;
    return p;
}
} // namespace context
