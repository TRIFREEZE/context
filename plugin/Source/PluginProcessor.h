#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Core/SharedServices.h"
#include "Audio/CaptureBuffer.h"
#include "Audio/PreviewPlayer.h"
#include "Analysis/AnalysisController.h"
#include "Generation/GenerationController.h"
#include "Generation/PromptBuilder.h"
#include "Audio/ContextRecorder.h"
#include "Audio/PreviewEngine.h"
#include "DSP/EffectChain.h"
#include "Session/SessionController.h"

namespace context
{
/** One Context instance in the DAW.

    Audio thread (processBlock): bit-exact passthrough, capture into the rolling
    buffer (try-lock, never waits), exact-bar context recording (preallocated, atomic
    hand-off), play-head read into atomics, optional preview mixes (single-file
    preview and the two-lane workspace engine + effect chain). Nothing else — no I/O,
    no network, no allocation, no logging.

    Everything else lives on worker threads owned by the controllers below. */
class ContextAudioProcessor final : public juce::AudioProcessor,
                                    private juce::ChangeListener,
                                    private juce::Timer
{
public:
    ContextAudioProcessor();
    ~ContextAudioProcessor() override;

    // ---- juce::AudioProcessor ----
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override;
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- services for the editor (message thread) ----
    SharedServices& services()            { return *shared; }
    GenerationController& generation()    { return *generationController; }
    AnalysisController& analysis()        { return *analysisController; }
    PreviewPlayer& preview()              { return previewPlayer; }
    const CaptureBuffer& capture() const  { return captureBuffer; }
    SessionController& session()          { return *sessionController; }
    fx::EffectChain& effects()            { return effectChain; }
    PreviewEngine& lanes()                { return previewEngine; }

    /** Per-project UI state, saved in the DAW project (lightweight only). */
    struct UiState
    {
        juce::String prompt;
        GenerationType type = GenerationType::OneShot;
        bool useTrackContext = true;
        bool sendAudioContext = false;
        juce::String contextMode = "layer";      // how the bridge uses the track context
        juce::String tab = "generate";
        juce::String selectedResultId;
    };
    UiState getUiState() const;
    void setUiState (const UiState&);

    struct HostInfo { std::optional<double> bpm; bool playing = false; double ppq = 0; int tsNum = 4, tsDen = 4; double sampleRate = 0.0; };
    HostInfo getHostInfo() const;

    PromptBuilder::Output previewPrompt (const UiState&) const;

    GenerationController::EnqueueResult generate();
    GenerationController::EnqueueResult regenerate (const GenerationResult& source);
    GenerationController::EnqueueResult variation (const GenerationResult& source);
    bool canVariation() const;
    /** Generates a new take in the same style (same prompt, type, tempo, key and context mode)
        and drops it into [from, to) of the output lane when it arrives. */
    GenerationController::EnqueueResult fillWithNewTake (double fromBeat, double toBeat);
    void analyzeTrack();

    std::optional<GenerationResult> getSelectedResult() const;
    void selectResult (const juce::String& id);

    /** Decodes on the file worker, then starts playback. */
    void previewResult (const GenerationResult&);
    juce::String getPreviewError() const;

    juce::String diagnosticsText() const;
    bool isLicensed() const;
    void rebuildProvider (bool force = false);

    /** Fired on the message thread when results/preview state change. */
    juce::ChangeBroadcaster stateChanged;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    GenerationRequest makeRequest (const UiState&) const;
    GenerationRequest requestFromResult (const GenerationResult&) const;
    juce::String providerKey (const Settings&) const;

    juce::SharedResourcePointer<SharedServices> shared;

    CaptureBuffer captureBuffer;
    PreviewPlayer previewPlayer;
    ContextRecorder contextRecorder;
    PreviewEngine previewEngine;
    fx::EffectChain effectChain;

    std::atomic<double> hostBpm { 0.0 };
    std::atomic<bool> hostPlaying { false };
    std::atomic<double> hostPpq { 0.0 };
    std::atomic<int> hostTsNum { 4 }, hostTsDen { 4 };
    std::atomic<bool> hostPlayingKnown { false };
    std::atomic<double> currentSampleRate { 0.0 };
    std::atomic<int> currentBlockSize { 0 };

    mutable juce::CriticalSection uiLock;
    UiState ui;
    juce::String previewError;
    juce::String currentProviderKey;
    int capturedSecondsSetting = 0;
    int timerTicks = 0;
    std::atomic<bool> alive { true };

    std::unique_ptr<AnalysisController> analysisController;
    std::unique_ptr<GenerationController> generationController;
    std::unique_ptr<SerialWorker> previewWorker;      // decodes preview files; joined before members it touches die
    std::unique_ptr<SessionController> sessionController;
    juce::String lastAutoLoadedResult;
    juce::String pendingFillJob;      // generation that will fill a selection
    void restoreSession (const juce::var& session, const juce::var& chain);

    JUCE_DECLARE_WEAK_REFERENCEABLE (ContextAudioProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ContextAudioProcessor)
};
} // namespace context
