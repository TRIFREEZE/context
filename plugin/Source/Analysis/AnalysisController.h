#pragma once

#include "TrackAnalyzer.h"
#include "Audio/CaptureBuffer.h"
#include "Core/SerialWorker.h"
#include "Generation/GenerationModels.h"
#include <juce_events/juce_events.h>

namespace context
{
/** Runs track analysis on its own worker thread. Analysis never touches the
    audio thread: the worker takes a snapshot of the CaptureBuffer and analyses
    the copy. Results stay on this computer. */
class AnalysisController : public juce::ChangeBroadcaster
{
public:
    enum class State { Idle, Analyzing, Done, NotEnoughAudio, Failed };

    AnalysisController (CaptureBuffer&, std::function<std::optional<double>()> hostBpmProvider);
    ~AnalysisController() override;

    /** Message thread. Returns false (and does nothing) if an analysis is already running. */
    bool requestAnalysis();

    State getState() const;
    juce::String getMessage() const;
    std::optional<TrackAnalysis> getLatest() const;
    void clear();

    /** Message thread: publish an analysis computed elsewhere (the captured 8-bar context),
        so prompts and the analysis readout use it. */
    void setExternalResult (const TrackAnalysis&);

    /** Generation worker: writes the most recent `seconds` of captured audio as a
        small mono 22.05 kHz 16-bit WAV for providers that accept audio context.
        Only called when the user explicitly enabled "Send Audio Context". */
    std::optional<AudioConditioningPayload> writeAudioContext (const juce::File& file, double seconds) const;

private:
    void run();

    CaptureBuffer& capture;
    std::function<std::optional<double>()> hostBpm;

    mutable juce::CriticalSection lock;
    State state = State::Idle;
    juce::String message;
    std::optional<TrackAnalysis> latest;

    SerialWorker worker { "Context analysis", 1 };   // last member: joined first
};
} // namespace context
