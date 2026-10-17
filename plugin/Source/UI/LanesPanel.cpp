#include "LanesPanel.h"
#include "Generation/Taste.h"
#include "PluginProcessor.h"

namespace context::ui
{
namespace
{
    juce::String bpmString (double bpm)
    {
        return juce::String (bpm, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    }

    void setupGain (juce::Slider& s, std::function<void (float)> onChange)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        s.setRange (-30.0, 6.0, 0.1);
        s.setDoubleClickReturnValue (true, 0.0);
        s.setSkewFactorFromMidPoint (-6.0);
        s.textFromValueFunction = [] (double v) { return (v > 0 ? "+" : "") + juce::String (v, 1) + " dB"; };
        s.setPopupDisplayEnabled (true, true, nullptr, 1200);
        s.setTooltip ("Audition level (double-click: 0 dB). Not printed into the exported file.");
        s.onValueChange = [&s, onChange] { onChange ((float) s.getValue()); };
    }
}

namespace
{
    struct GridChoice { const char* label; double beats; };   // beats < 0 = one bar
    const GridChoice gridChoices[] = { { "Bar", -1.0 }, { "1/2", 2.0 }, { "Beat", 1.0 }, { "1/8", 0.5 }, { "1/16", 0.25 }, { "Off", 0.0 } };
}

LanesPanel::LanesPanel (ContextAudioProcessor& p) : processor (p), overlay (p.session())
{
    addAndMakeVisible (ruler);
    addAndMakeVisible (contextLane);
    addAndMakeVisible (outputLane);
    outputLane.addAndMakeVisible (overlay);
    addChildComponent (selectionBar);
    ruler.onSeek = [this] (juce::int64 s) { processor.session().seek (s); };

    overlay.onSeek = [this] (double beat)
    {
        const auto t = processor.session().sessionTempo();
        processor.session().seek (edit::samplesForBeats (beat, t, processor.session().renderRate()));
    };
    overlay.onSelectionChanged = [this] { updateSelectionBar(); };
    overlay.onContextMenu = [this] (juce::Point<int> pt) { showEditMenu (pt); };
    selectionBar.onNewTake = [this] { fillWithNewTake(); };
    selectionBar.onFromTake = [this] { showFillFromMenu (&selectionBar); };
    selectionBar.onSplit = [this] { splitAtSelectionOrPlayhead(); };
    selectionBar.onDelete = [this] { deleteSelection(); };

    for (int i = 0; i < (int) std::size (gridChoices); ++i)
        snap.addItem (gridChoices[i].beats == 0.0 ? juce::String ("No snap") : juce::String ("Snap ") + gridChoices[i].label, i + 1);
    snap.setTooltip ("Grid for selecting, moving and trimming (G turns snapping on and off; hold Alt to drag freely)");
    snap.onChange = [this]
    {
        const int i = snap.getSelectedId() - 1;
        if (! juce::isPositiveAndBelow (i, (int) std::size (gridChoices)))
            return;
        const double bpb = musical::quarterNotesPerBar (processor.session().sessionTempo().signature);
        processor.session().setGrid (gridChoices[i].beats < 0.0 ? bpb : gridChoices[i].beats);
    };
    addAndMakeVisible (snap);

    edgeMode.setItems ({ "Trim", "Stretch" },
                       { "Dragging a clip edge trims it (Alt+drag stretches). T switches.",
                         "Dragging a clip edge time-stretches it, pitch kept (Alt+drag trims). T switches." });
    edgeMode.setFontSize (type::label);
    edgeMode.onChange = [this] (int i) { setStretchEdges (i == 1); };
    addAndMakeVisible (edgeMode);

    setStyle (takesButton, ButtonStyle::Ghost);
    takesButton.setTooltip ("Takes: every version of this sound. Pick which one plays, or comp bars from several.");
    takesButton.onClick = [this] { showTakesMenu(); };
    addAndMakeVisible (takesButton);

    contextLane.setTooltip ("Your track: the 8 bars captured from the DAW");
    contextLane.onRightClick = [this] (const juce::MouseEvent&) { showContextMenu(); };
    contextLane.onClick = [this]
    {
        if (! processor.session().hasContext() && ! processor.session().captureStatus().busy())
            processor.session().startCapture();
    };

    outputLane.onFileDropped = [this] (const juce::File& f) { processor.session().importFile (f); };
    overlay.onDragOut = [this]() -> juce::File
    {
        juce::MouseCursor::showWaitCursor();
        const auto r = processor.session().exportProcessed();
        juce::MouseCursor::hideWaitCursor();
        if (! r.ok())
        {
            processor.session().notify (r.error, true);
            return {};
        }
        if (auto res = processor.getSelectedResult(); res && processor.session().getState().sourceKind == SessionState::SourceKind::Generated)
            processor.services().history().setPinned (res->generationId);
        return r.file;
    };

    setStyle (captureButton, ButtonStyle::Secondary);
    captureButton.setTooltip ("Press, then play your DAW: Context records exactly 8 bars from the next bar line.");
    captureButton.onClick = [this]
    {
        auto& s = processor.session();
        if (s.captureStatus().armed() || s.captureStatus().recording()) s.cancelCapture();
        else s.startCapture();
    };
    addAndMakeVisible (captureButton);

    setStyle (loadButton, ButtonStyle::Secondary);
    loadButton.setTooltip ("Load your own sample (or drop an audio file onto the lane)");
    loadButton.onClick = [this] { openSampleChooser(); };
    addAndMakeVisible (loadButton);

    setStyle (retargetButton, ButtonStyle::Toggle);
    retargetButton.onClick = [this] { processor.session().retargetContextToHostTempo(); };
    retargetButton.setTooltip ("Time-stretch the captured context to the DAW's current tempo");
    setStyle (recaptureButton, ButtonStyle::Secondary);
    recaptureButton.onClick = [this] { processor.session().startCapture(); };
    addChildComponent (retargetButton);
    addChildComponent (recaptureButton);

    ctxMute.setTooltip ("Mute the context lane");
    ctxSolo.setTooltip ("Solo the context lane");
    outMute.setTooltip ("Mute the sound lane");
    outSolo.setTooltip ("Solo the sound lane");
    ctxMute.onClick = [this] { processor.session().setLaneMute (PreviewEngine::contextLane, ctxMute.getToggleState()); };
    ctxSolo.onClick = [this] { processor.session().setLaneSolo (PreviewEngine::contextLane, ctxSolo.getToggleState()); };
    outMute.onClick = [this] { processor.session().setLaneMute (PreviewEngine::outputLane, outMute.getToggleState()); };
    outSolo.onClick = [this] { processor.session().setLaneSolo (PreviewEngine::outputLane, outSolo.getToggleState()); };
    setupGain (ctxGain, [this] (float db) { processor.session().setLaneGainDb (PreviewEngine::contextLane, db); });
    setupGain (outGain, [this] (float db) { processor.session().setLaneGainDb (PreviewEngine::outputLane, db); });
    for (juce::Component* c : { (juce::Component*) &ctxMute, (juce::Component*) &ctxSolo, (juce::Component*) &outMute, (juce::Component*) &outSolo,
                                (juce::Component*) &ctxGain, (juce::Component*) &outGain, (juce::Component*) &ctxMenu, (juce::Component*) &outMenu })
        addAndMakeVisible (c);
    ctxMenu.onClick = [this] { showContextMenu(); };
    outMenu.onClick = [this] { showOutputMenu(); };

    verdict.onKeep = [this] { keepGeneration (verdict.generationId()); };
    verdict.onPass = [this] (const juce::StringArray& why) { passGeneration (verdict.generationId(), verdict.takeId(), why); };
    verdict.onNewTake = [this]
    {
        if (const auto r = processor.services().history().find (verdict.generationId()))
        {
            const auto e = processor.regenerate (*r);
            if (! e.ok)
                processor.session().notify (e.error.userMessage, true);
        }
    };
    verdict.onLayoutChanged = [this] { layoutVerdict(); };
    addChildComponent (verdict);

    bpmChip.onClick = [this] { showTempoSheet (*this, processor); };
    keyChip.onClick = [this] { showKeySheet (*this, processor); };
    addAndMakeVisible (bpmChip);
    addAndMakeVisible (keyChip);

    juce::StringArray modes, tips;
    for (auto m : allFitModes)
    {
        modes.add (toLabel (m));
        tips.add (tooltipFor (m));
    }
    fitMode.setItems (modes, tips);
    fitMode.setProportional (true);
    fitMode.onChange = [this] (int i) { processor.session().setFitMode (allFitModes[i]); };
    addAndMakeVisible (fitMode);

    for (int i = 0; i < (int) std::size (stretch::allAlgorithms); ++i)
        algorithm.addItem (stretch::toLabel (stretch::allAlgorithms[i]), i + 1);
    algorithm.setTooltip ("Stretch algorithm. Auto analyses the sound.");
    algorithm.onChange = [this]
    {
        const int i = algorithm.getSelectedId() - 1;
        if (juce::isPositiveAndBelow (i, (int) std::size (stretch::allAlgorithms)))
        {
            processor.session().setAlgorithm (stretch::allAlgorithms[i]);
            algorithm.setTooltip (stretch::tooltipFor (stretch::allAlgorithms[i]));
        }
    };
    addAndMakeVisible (algorithm);

    pitch.setSliderStyle (juce::Slider::LinearHorizontal);
    pitch.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    pitch.setRange (-12.0, 12.0, 0.1);
    pitch.setDoubleClickReturnValue (true, 0.0);
    pitch.textFromValueFunction = [] (double v) { return (v > 0 ? "+" : "") + juce::String (v, 1) + " st"; };
    pitch.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters ("-0123456789.").getDoubleValue(); };
    pitch.setTooltip ("Transpose without changing length (double-click: 0)");
    pitch.onDragEnd = [this] { processor.session().setPitch (pitch.getValue()); };
    pitch.onValueChange = [this]
    {
        repaint (pitchValue);
        if (! pitch.isMouseButtonDown()) processor.session().setPitch (pitch.getValue());
    };
    addAndMakeVisible (pitch);

    refresh();
}

LanesPanel::~LanesPanel() = default;

TimelineView LanesPanel::computeTimeline() const
{
    auto& s = processor.session();
    TimelineView v;
    v.sampleRate = s.renderRate();
    v.tempo = s.sessionTempo();
    const auto ctx = s.contextLane();
    const auto out = s.outputLane();
    juce::int64 ctxLen = 0;
    if (ctx.hasAudio && ctx.sampleRate > 0.0)
        ctxLen = (juce::int64) std::llround ((double) ctx.lengthSamples * v.sampleRate / ctx.sampleRate);
    v.lengthSamples = juce::jmax (ctxLen, out.hasAudio ? out.lengthSamples : (juce::int64) 0);
    if (v.lengthSamples <= 0)
        v.lengthSamples = musical::samplesForBars (juce::jmax (s.getState().lengthBars, ctx.hasAudio ? 0 : 8), v.tempo, v.sampleRate);
    return v;
}

juce::String LanesPanel::outputDetail() const
{
    auto& s = processor.session();
    const auto src = s.outputSource();
    if (src == nullptr)
        return {};
    juce::String d = src->origin == AudioAsset::Origin::Generated ? (src->alignedToContext ? "Made on your track, " : "Generated, ") : "Your file, ";
    d << juce::String (src->seconds(), src->seconds() < 10.0 ? 2 : 1) << " s";
    const auto fit = s.fitStatus();
    if (fit.hasReport && fit.report.sourceBpm)
        d << " at " << juce::roundToInt (*fit.report.sourceBpm) << " BPM";
    return d;
}

juce::String LanesPanel::bpmLabel() const
{
    const auto src = processor.session().tempoSource();
    return juce::String ("BPM") + (src == SessionController::TempoSource::Manual || src == SessionController::TempoSource::Set ? " (set)" : "");
}

juce::String LanesPanel::fitSummary() const
{
    auto& s = processor.session();
    const auto fit = s.fitStatus();
    if (s.outputSource() == nullptr)
        return "Generate or drop a sample; it will be fitted to exactly " + juce::String (s.getState().lengthBars) + " bars.";
    if (fit.rendering)
        return "Fitting to " + juce::String (s.getState().lengthBars) + " bars...";
    if (fit.error.isNotEmpty())
        return fit.error;
    if (! fit.hasReport)
        return {};
    // One plain sentence about what was done to the sound.
    const auto& r = fit.report;
    auto pct = [] (double p) { return (p >= 0 ? "+" : "") + juce::String (p, 1) + "%"; };
    juce::String t;
    t << juce::String (r.sourceBars, 2) << glyph (" \xe2\x86\x92 ") << r.targetBars << " bars: ";
    switch (r.modeUsed)
    {
        case FitMode::Stretch: t << "stretched " << pct (r.stretchPercent); break;
        case FitMode::Repitch: t << "repitched " << (r.pitchSemitones >= 0 ? "+" : "") << juce::String (r.pitchSemitones, 1) << " st"; break;
        case FitMode::Loop:    t << "looped " << r.loopRepeats << glyph ("\xc3\x97"); if (r.engineUsed) t << ", each repeat " << pct (r.stretchPercent); break;
        case FitMode::Trim:
            if (r.aligned && r.alignedStartBar > 0)
                t << "kept bars " << (r.alignedStartBar + 1) << glyph ("\xe2\x80\x93") << (r.alignedStartBar + r.targetBars)
                  << " (the first bars were empty), in time with your track";
            else
                t << (r.aligned ? "kept in time with your track" : r.sourceBars < r.targetBars ? "padded" : "trimmed");
            break;
        case FitMode::Auto:    break;
    }
    if (r.engineUsed && r.modeUsed != FitMode::Repitch)
        t << " with " << stretch::toLabel (r.algorithmUsed);
    if (std::abs (r.pitchSemitones) > 0.05 && r.modeUsed != FitMode::Repitch)
        t << ", " << (r.pitchSemitones > 0 ? "+" : "") << juce::String (r.pitchSemitones, 1) << " st";
    if (r.trimmedStartSeconds + r.trimmedEndSeconds > 0.005)
        t << ", " << juce::String (r.trimmedStartSeconds + r.trimmedEndSeconds, 2) << " s of silence removed";
    if (s.getState().fit.mode == FitMode::Auto)
        t << ". Auto picked " << toLabel (r.modeUsed) << ".";
    return t;
}

void LanesPanel::refresh()
{
    auto& s = processor.session();
    const auto st = s.getState();
    updateVerdict();
    timeline = computeTimeline();
    ruler.setTimeline (timeline);
    contextLane.setTimeline (timeline);
    outputLane.setTimeline (timeline);

    const auto ctx = s.contextLane();
    juce::int64 ctxLen = 0;
    if (ctx.hasAudio && ctx.sampleRate > 0)
        ctxLen = (juce::int64) std::llround ((double) ctx.lengthSamples * timeline.sampleRate / ctx.sampleRate);
    if (ctx.peaks != nullptr && lastContextPeaks == nullptr)
        contextLane.revealNext();                  // a fresh capture draws itself in
    lastContextPeaks = ctx.peaks;
    contextLane.setPeaks (ctx.peaks, ctxLen);
    contextLane.setMissing (ctx.missing ? ctx.message : juce::String());
    const auto cap = s.captureStatus();
    if (cap.armed())
        contextLane.setEmptyText ("Press play in your DAW", "Recording starts on the next bar line and stops after exactly 8 bars");
    else
        contextLane.setEmptyText ("Capture your track", "Click Capture, then play your DAW. Context records exactly 8 bars.");
    contextLane.setBusy (cap.processing && ! cap.recording(), "Analysing the capture...");

    const auto out = s.outputLane();
    if (st.baseTake != lastBaseTake && out.hasAudio)
    {
        lastBaseTake = st.baseTake;
        outputLane.revealNext();                   // a new take draws itself in
    }
    outputLane.setPeaks (out.peaks, out.hasAudio ? out.lengthSamples : 0);
    outputLane.setMissing (out.missing ? out.message : juce::String());
    outputLane.setEmptyText ("Drop audio here", "or generate something above");
    overlay.setTimeline (timeline);
    overlay.update();
    {
        const auto v = s.editView();
        int choice = (int) std::size (gridChoices) - 1;
        for (int i = 0; i < (int) std::size (gridChoices); ++i)
        {
            const double b = gridChoices[i].beats < 0.0 ? v.beatsPerBar : gridChoices[i].beats;
            if (std::abs (b - v.gridBeats) < 1.0e-9) { choice = i; break; }
        }
        snap.setSelectedId (choice + 1, juce::dontSendNotification);
        const bool stretchEdges = processor.getUiState().stretchEdges;
        overlay.setStretchEdges (stretchEdges);
        edgeMode.setSelected (stretchEdges ? 1 : 0);
        const auto takes = s.takes();
        int baseNumber = 0;
        for (const auto& t : takes) if (t.isBase) baseNumber = t.number;
        takesButton.setVisible (! takes.empty());
        takesButton.setButtonText (takes.size() > 1 ? "Take " + juce::String (baseNumber) + " of " + juce::String ((int) takes.size())
                                                    : juce::String ("Take ") + juce::String (baseNumber));
    }
    updateSelectionBar();

    // Header controls
    const bool capBusy = cap.armed() || cap.recording();
    captureButton.setButtonText (cap.recording() ? "Bar " + juce::String (juce::jmin (cap.barsDone + 1, cap.targetBars)) + " / " + juce::String (cap.targetBars)
                                 : cap.armed() ? "Waiting..." : s.hasContext() ? "Recapture" : "Capture");
    setStyle (captureButton, capBusy ? ButtonStyle::Danger : ButtonStyle::Secondary);
    captureButton.setTooltip (capBusy ? "Click to cancel the capture" : "Press, then play your DAW: Context records exactly 8 bars from the next bar line.");

    ctxMute.setToggleState (st.contextMute, juce::dontSendNotification);
    ctxSolo.setToggleState (st.contextSolo, juce::dontSendNotification);
    outMute.setToggleState (st.outputMute, juce::dontSendNotification);
    outSolo.setToggleState (st.outputSolo, juce::dontSendNotification);
    if (! ctxGain.isMouseButtonDown()) ctxGain.setValue (st.contextGainDb, juce::dontSendNotification);
    if (! outGain.isMouseButtonDown()) outGain.setValue (st.outputGainDb, juce::dontSendNotification);

    {
        const auto src = s.tempoSource();
        bpmChip.setText (bpmString (s.sessionTempo().bpm), numberFont (type::readout - 2.0f, Weight::SemiBold), colours::text);
        bpmChip.setUnit (bpmLabel());
        bpmChip.setTooltip (src == SessionController::TempoSource::Context ? "Your track's tempo. Click for half-time / double-time or another tempo."
                            : src == SessionController::TempoSource::Host ? "The DAW's tempo. Click to use another one."
                            : "The tempo you set. Click to change it.");
        const auto key = s.sessionKey();
        keyChip.setText (key.known() ? key.display() : juce::String ("Set key"), font (type::title, Weight::Medium),
                         key.known() ? colours::text : colours::faint);
        keyChip.setTooltip (key.chosen ? "The key you set. Click to change it, or go back to detecting it."
                            : key.known() ? "Detected key (" + juce::String (juce::roundToInt (key.confidence * 100.0)) + "% sure). Click to change it."
                            : "No clear key detected yet. Click to set one.");
        layoutLaneHeader (ctxHeader, true);
    }

    tempoMismatch = s.hostTempoDiffers();
    retargetButton.setVisible (tempoMismatch);
    recaptureButton.setVisible (tempoMismatch);
    if (tempoMismatch)
        retargetButton.setButtonText ("Fit to " + bpmString (*s.hostBpm()) + " BPM");

    for (int i = 0; i < (int) std::size (allFitModes); ++i)
        if (allFitModes[i] == st.fit.mode)
            fitMode.setSelected (i);
    for (int i = 0; i < (int) std::size (stretch::allAlgorithms); ++i)
        if (stretch::allAlgorithms[i] == st.fit.algorithm)
            algorithm.setSelectedId (i + 1, juce::dontSendNotification);
    const bool repitch = st.fit.mode == FitMode::Repitch || st.fit.algorithm == stretch::Algorithm::Repitch;
    algorithm.setEnabled (st.fit.mode != FitMode::Repitch);
    pitch.setEnabled (! repitch);
    pitch.setTooltip (repitch ? "Repitch ties pitch to tempo; choose another mode to transpose independently" : "Transpose without changing length (double-click: 0)");
    if (! pitch.isMouseButtonDown())
        pitch.setValue (st.fit.pitchSemitones, juce::dontSendNotification);
    const bool hasSource = s.outputSource() != nullptr;
    fitMode.setEnabled (hasSource);
    algorithm.setEnabled (hasSource && st.fit.mode != FitMode::Repitch);
    pitch.setEnabled (hasSource && ! repitch);

    resized();
    repaint();
}

void LanesPanel::tick()
{
    auto& s = processor.session();
    const bool playing = s.isPlaying();
    const auto pos = s.playheadSamples();
    // Shown while playing, and parked where the user clicked while stopped.
    const bool showHead = playing || pos > 0;
    ruler.setPlayhead (pos, showHead);
    contextLane.setPlayhead (pos, showHead && contextLane.hasAudio());
    outputLane.setPlayhead (pos, showHead && outputLane.hasAudio());
    overlay.setPlayheadBeat (playheadBeat(), showHead);

    const auto cap = s.captureStatus();
    contextLane.setCaptureProgress (cap.progress, cap.recording());
    if (cap.recording() != wasRecording || (cap.recording() && cap.barsDone != lastBarsShown))
    {
        wasRecording = cap.recording();
        lastBarsShown = cap.barsDone;
        refresh();
    }
    {
        // A new take on the way (a fill shows in its range instead, on the overlay).
        auto& gen = processor.generation();
        const auto job = gen.getCurrentJob();
        const bool active = gen.isBusy() && ! s.editView().pendingFill.has_value();
        outputLane.setGenerating (active, active ? job.statusText : juce::String(), active ? job.progress : -1.0);
    }
    const auto fit = s.fitStatus();
    outputLane.setBusy (fit.rendering || s.outputLane().loading, fit.rendering ? "Fitting to " + juce::String (s.getState().lengthBars) + " bars" : "Loading...",
                        fit.rendering ? (double) fit.progress : -1.0);
    if (fit.rendering)
        repaint (fitInfo);
}

void LanesPanel::openSampleChooser()
{
    chooser = std::make_unique<juce::FileChooser> ("Load a sample into Context", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   assets::supportedWildcard());
    juce::Component::SafePointer<LanesPanel> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
    {
        if (safe != nullptr && fc.getResult().existsAsFile())
            safe->processor.session().importFile (fc.getResult());
    });
}

void LanesPanel::showContextMenu()
{
    auto& s = processor.session();
    const auto cap = s.captureStatus();
    juce::PopupMenu m;
    m.addItem (1, s.hasContext() ? "Capture again (8 bars)" : "Capture 8 bars", ! cap.busy());
    m.addItem (2, "Cancel capture", cap.armed() || cap.recording());
    m.addItem (3, "Quick analyze (last 8 s of playback)");
    if (s.hostTempoDiffers())
        m.addItem (5, "Fit context to the DAW tempo");
    m.addSeparator();
    m.addItem (4, "Clear captured context", s.hasContext() || s.getState().context.has_value());
    juce::Component::SafePointer<LanesPanel> safe (this);
    showMenu (m, *this, juce::PopupMenu::Options().withTargetComponent (&ctxMenu), [safe] (int r)
    {
        if (safe == nullptr) return;
        auto& ss = safe->processor.session();
        if (r == 1) ss.startCapture();
        if (r == 2) ss.cancelCapture();
        if (r == 3) safe->processor.analyzeTrack();
        if (r == 4) ss.clearContext();
        if (r == 5) ss.retargetContextToHostTempo();
    });
}

void LanesPanel::addOutputItems (juce::PopupMenu& m, int base)
{
    auto& s = processor.session();
    const auto src = s.outputSource();
    m.addItem (base + 1, "Load sample...");
    m.addItem (base + 2, "Restore generated version", s.canRestoreGenerated());
    m.addItem (base + 5, "Reset fit settings");
    m.addItem (base + 6, "Reset edits", s.getState().arrangement.isEdited());
    m.addSeparator();
    m.addItem (base + 3, "Show original file", src != nullptr && src->sourceFile.existsAsFile());
    m.addItem (base + 4, "Clear the lane", src != nullptr || s.outputLane().missing);
}

void LanesPanel::handleOutputItem (int r)
{
    auto& ss = processor.session();
    const auto src = ss.outputSource();
    if (r == 1) openSampleChooser();
    if (r == 2) ss.restoreGenerated();
    if (r == 3 && src != nullptr) src->sourceFile.revealToUser();
    if (r == 4) ss.clearOutput();
    if (r == 5) ss.resetFit();
    if (r == 6) ss.editReset();
}

void LanesPanel::showOutputMenu()
{
    juce::PopupMenu m;
    addOutputItems (m, 0);
    juce::Component::SafePointer<LanesPanel> safe (this);
    showMenu (m, *this, juce::PopupMenu::Options().withTargetComponent (&outMenu), [safe] (int r)
    {
        if (safe != nullptr && r > 0) safe->handleOutputItem (r);
    });
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

double LanesPanel::playheadBeat() const
{
    auto& s = processor.session();
    const auto t = s.sessionTempo();
    const double rate = s.renderRate();
    const double total = s.editView().totalBeats;
    double beat = (double) s.playheadSamples() * t.bpm / (60.0 * rate);
    if (total > 0.0)
        beat = std::fmod (beat, total);
    return beat;
}

juce::String LanesPanel::rangeText (double from, double to) const
{
    const auto v = processor.session().editView();
    auto pos = [&] (double beat)
    {
        // bar.beat, 1-based, like the transport
        const int bar = (int) std::floor (beat / v.beatsPerBar + 1.0e-9);
        const double inBar = beat - bar * v.beatsPerBar;
        const double beatNum = inBar + 1.0;
        return juce::String (bar + 1) + "." + (std::abs (beatNum - std::round (beatNum)) < 1.0e-6 ? juce::String ((int) std::round (beatNum))
                                                                                                   : juce::String (beatNum, 2));
    };
    const double bars = (to - from) / v.beatsPerBar;
    juce::String len = std::abs (bars - std::round (bars)) < 1.0e-6 ? juce::String ((int) std::round (bars)) + ((int) std::round (bars) == 1 ? " bar" : " bars")
                                                                    : juce::String (to - from, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " beats";
    return pos (from) + glyph (" \xe2\x80\x93 ") + pos (to) + "  (" + len + ")";
}

void LanesPanel::updateVerdict()
{
    // Ask once about each new generated take that becomes the lane's sound and has no verdict.
    if (verdict.isVisible() && ! verdict.isAsking())
        return;                                    // showing "Kept" / "Passed": let it finish
    auto& s = processor.session();
    juce::String gen, takeId;
    int number = 0;
    for (const auto& t : s.takes())
        if (t.isBase && t.ready && t.generationId.isNotEmpty())
        {
            gen = t.generationId;
            takeId = t.id;
            number = t.number;
        }
    if (gen.isEmpty())
    {
        verdict.dismissQuestion();
        return;
    }
    if (gen == verdict.generationId() && verdict.isAsking())
        return;
    const auto r = processor.services().history().find (gen);
    if (! r || taste::ratingOf (*r) != 0 || verdictAsked.count (gen) > 0)
    {
        verdict.dismissQuestion();
        return;
    }
    verdictAsked.insert (gen);
    verdict.ask (gen, takeId, "Take " + juce::String (number));
    layoutVerdict();
    verdict.toFront (false);
}

void LanesPanel::layoutVerdict()
{
    // Floats over the lower part of the sound lane, centred (the selection bar uses the top edge).
    const auto lane = outputLane.getBounds();
    const int w = juce::jmin (verdict.idealWidth(), lane.getWidth() - 16), h = 36;
    verdict.setBounds (juce::Rectangle<int> (w, h).withCentre ({ lane.getCentreX(), lane.getBottom() - h / 2 - 10 }));
}

void LanesPanel::keepGeneration (const juce::String& gen)
{
    if (gen.isEmpty()) return;
    processor.services().history().setRating (gen, 1);
    verdictAsked.insert (gen);
    if (gen == verdict.generationId() && verdict.isVisible())
        verdict.showKept();
    else
        processor.session().notify ("Kept: saved to Favorites.");
}

void LanesPanel::passGeneration (const juce::String& gen, const juce::String& takeId, const juce::StringArray& why)
{
    if (gen.isEmpty()) return;
    auto& history = processor.services().history();
    history.setRating (gen, -1, why);
    verdictAsked.insert (gen);
    const auto r = history.find (gen);
    if (takeId.isNotEmpty())
        processor.session().passTake (takeId);
    const auto steer = r ? taste::forPrompt (history.recent (300), r->prompt, r->type) : taste::Steer{};
    if (gen == verdict.generationId() && verdict.isVisible())
        verdict.showPassed (steer.labels, processor.isLicensed() && ! processor.generation().isBusy());
    else
        processor.session().notify (steer.labels.isEmpty() ? juce::String ("Passed.") : "Passed. Next takes: " + steer.labels.joinIntoString (", ").toLowerCase());
}

void LanesPanel::updateSelectionBar()
{
    const auto sel = overlay.selection();
    if (! sel || ! isVisible())
    {
        selectionBar.hide();
        return;
    }
    auto& gen = processor.generation();
    const bool idle = gen.getCurrentJob().state != JobState::Generating && gen.getCurrentJob().state != JobState::Submitting;
    const bool canGenerate = processor.isLicensed() && idle && ! processor.session().editView().pendingFill;
    selectionBar.showFor ({}, rangeText (sel->first, sel->second), canGenerate);
    // Above the lane, centred on the selection, kept inside the lane.
    const int w = selectionBar.preferredWidth(), h = 30;
    const auto lane = outputLane.getBounds();
    const auto t = processor.session().sessionTempo();
    const double rate = processor.session().renderRate();
    auto xFor = [&] (double beat) { return timeline.xForSample (beat * 60.0 / t.bpm * rate, lane.toFloat()); };
    const int cx = juce::roundToInt ((xFor (sel->first) + xFor (sel->second)) * 0.5f);
    auto r = juce::Rectangle<int> (w, h).withCentre ({ cx, lane.getY() - h / 2 - 2 });
    r = r.withX (juce::jlimit (lane.getX(), juce::jmax (lane.getX(), lane.getRight() - w), r.getX()));
    selectionBar.showFor (r, rangeText (sel->first, sel->second), canGenerate);
    selectionBar.toFront (false);
}

void LanesPanel::splitAtSelectionOrPlayhead()
{
    auto& s = processor.session();
    if (auto sel = overlay.selection())
    {
        s.editSplit (sel->first);
        s.editSplit (sel->second);
    }
    else
        s.editSplit (overlay.snapBeat (playheadBeat(), true));
}

void LanesPanel::deleteSelection()
{
    if (auto sel = overlay.selection())
    {
        processor.session().editDelete (sel->first, sel->second);
        overlay.setSelection (std::nullopt);
    }
}

void LanesPanel::fillWithNewTake()
{
    auto sel = overlay.selection();
    if (! sel)
        return;
    auto res = processor.fillWithNewTake (sel->first, sel->second);
    if (! res.ok)
        processor.session().notify (res.error.userMessage.isNotEmpty() ? res.error.userMessage : ProviderError::defaultUserMessage (res.error.code), true);
    else
        overlay.setSelection (std::nullopt);
}

void LanesPanel::showTakesMenu()
{
    auto& s = processor.session();
    const auto takes = s.takes();
    juce::PopupMenu m;
    m.addSectionHeader ("Plays everywhere");
    for (int i = 0; i < (int) takes.size(); ++i)
    {
        const auto& t = takes[(size_t) i];
        juce::String label = "Take " + juce::String (t.number) + "   " + fitText (t.name, font (type::body), 220.0f);
        if (t.missing) label << "  (missing)";
        m.addItem (100 + i, label, ! t.missing && ! t.loading, t.isBase);
    }
    juce::PopupMenu remove;
    for (int i = 0; i < (int) takes.size(); ++i)
        if (! takes[(size_t) i].isBase)
            remove.addItem (200 + i, "Take " + juce::String (takes[(size_t) i].number) + (takes[(size_t) i].used ? "  (its bars go silent)" : ""));
    m.addSeparator();
    m.addSubMenu ("Remove a take", remove, remove.getNumItems() > 0);
    juce::Component::SafePointer<LanesPanel> safe (this);
    showMenu (m, *this, juce::PopupMenu::Options().withTargetComponent (&takesButton), [safe, takes] (int r)
    {
        if (safe == nullptr) return;
        if (r >= 100 && r < 100 + (int) takes.size()) safe->processor.session().useTakeEverywhere (takes[(size_t) r - 100].id);
        if (r >= 200 && r < 200 + (int) takes.size()) safe->processor.session().removeTake (takes[(size_t) r - 200].id);
    });
}

void LanesPanel::showFillFromMenu (juce::Component* target)
{
    auto sel = overlay.selection();
    if (! sel)
        return;
    const auto takes = processor.session().takes();
    juce::PopupMenu m;
    for (int i = 0; i < (int) takes.size(); ++i)
        m.addItem (100 + i, "Take " + juce::String (takes[(size_t) i].number) + "   " + fitText (takes[(size_t) i].name, font (type::body), 220.0f),
                   takes[(size_t) i].ready);
    juce::Component::SafePointer<LanesPanel> safe (this);
    const auto range = *sel;
    showMenu (m, *this, juce::PopupMenu::Options().withTargetComponent (target), [safe, takes, range] (int r)
    {
        if (safe != nullptr && r >= 100 && r < 100 + (int) takes.size())
            safe->processor.session().editFill (range.first, range.second, takes[(size_t) r - 100].id);
    });
}

void LanesPanel::showEditMenu (juce::Point<int> at)
{
    auto& s = processor.session();
    const auto v = s.editView();
    const auto sel = overlay.selection();
    const auto t = s.sessionTempo();
    const double rate = s.renderRate();
    const double beatAtMouse = (double) at.x / juce::jmax (1, overlay.getWidth()) * (double) timeline.lengthSamples * t.bpm / (60.0 * rate);
    const int clip = edit::clipAt (s.getState().arrangement, v.baseTake, v.totalBeats, beatAtMouse);
    const auto takes = s.takes();

    juce::PopupMenu m;
    m.addItem (1, sel ? "Split at selection edges" : "Split at playhead", true, false);
    m.addItem (2, "Delete selection", sel.has_value());
    m.addSeparator();
    m.addItem (3, "Fill selection with a new take", sel.has_value() && processor.isLicensed() && ! v.pendingFill);
    juce::PopupMenu from;
    for (int i = 0; i < (int) takes.size(); ++i)
        from.addItem (100 + i, "Take " + juce::String (takes[(size_t) i].number), sel.has_value() && takes[(size_t) i].ready);
    m.addSubMenu ("Fill selection from", from, sel.has_value() && takes.size() > 1);
    if (v.pendingFill)
        m.addItem (9, "Cancel the pending fill");
    m.addSeparator();
    m.addItem (4, "Cut", sel.has_value());
    m.addItem (5, "Copy", sel.has_value());
    m.addItem (6, "Paste at playhead", s.canPaste());
    m.addItem (7, "Duplicate selection", sel.has_value());
    if (clip >= 0)
    {
        m.addSeparator();
        juce::PopupMenu gain;
        const float gains[] = { 6.0f, 3.0f, 0.0f, -3.0f, -6.0f, -12.0f };
        for (int i = 0; i < (int) std::size (gains); ++i)
            gain.addItem (300 + i, (gains[i] > 0 ? "+" : "") + juce::String (gains[i], 0) + " dB", true,
                          std::abs (v.clips[(size_t) clip].gainDb - gains[i]) < 0.01f);
        m.addSubMenu ("Clip level", gain);
        for (const auto& tk : takes)
            if (tk.id == v.clips[(size_t) clip].take && ! tk.isBase)
                m.addItem (8, "Use take " + juce::String (tk.number) + " everywhere");
        for (const auto& tk : takes)
            if (tk.id == v.clips[(size_t) clip].take && tk.generationId.isNotEmpty())
            {
                const auto r = processor.services().history().find (tk.generationId);
                const int rating = r ? taste::ratingOf (*r) : 0;
                m.addSeparator();
                m.addItem (20, "Keep take " + juce::String (tk.number), rating <= 0, rating > 0);
                m.addItem (21, "Pass on take " + juce::String (tk.number) + "...");
            }
    }
    m.addSeparator();
    addOutputItems (m, 1000);

    juce::Component::SafePointer<LanesPanel> safe (this);
    showMenu (m, *this, juce::PopupMenu::Options().withTargetScreenArea ({ overlay.localPointToGlobal (at), { 1, 1 } }),
                     [safe, sel, clip, takes, v] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& ss = safe->processor.session();
        switch (r)
        {
            case 1: safe->splitAtSelectionOrPlayhead(); break;
            case 2: safe->deleteSelection(); break;
            case 3: safe->fillWithNewTake(); break;
            case 4: if (sel) { ss.editCut (sel->first, sel->second); safe->overlay.setSelection (std::nullopt); } break;
            case 5: if (sel) ss.editCopy (sel->first, sel->second); break;
            case 6: ss.editPaste (safe->overlay.snapBeat (safe->playheadBeat(), false)); break;
            case 7: if (sel) ss.editDuplicate (sel->first, sel->second); break;
            case 8: if (clip >= 0) ss.useTakeEverywhere (v.clips[(size_t) clip].take); break;
            case 9: ss.cancelPendingFill(); break;
            case 20:
            case 21:
                if (clip >= 0)
                    for (const auto& tk : takes)
                        if (tk.id == v.clips[(size_t) clip].take && tk.generationId.isNotEmpty())
                        {
                            if (r == 20)
                                safe->keepGeneration (tk.generationId);
                            else
                            {
                                safe->verdict.askWhy (tk.generationId, tk.id, "Take " + juce::String (tk.number));
                                safe->layoutVerdict();
                                safe->verdict.toFront (false);
                            }
                        }
                break;
            default: break;
        }
        if (r >= 100 && r < 100 + (int) takes.size() && sel)
            ss.editFill (sel->first, sel->second, takes[(size_t) r - 100].id);
        if (r >= 300 && r < 306 && clip >= 0)
        {
            const float gains[] = { 6.0f, 3.0f, 0.0f, -3.0f, -6.0f, -12.0f };
            ss.editClipGain (clip, gains[r - 300]);
        }
        if (r > 1000)
            safe->handleOutputItem (r - 1000);
    });
}

void LanesPanel::setStretchEdges (bool on)
{
    auto ui = processor.getUiState();
    if (ui.stretchEdges != on)
    {
        ui.stretchEdges = on;
        processor.setUiState (ui);
    }
    overlay.setStretchEdges (on);
    edgeMode.setSelected (on ? 1 : 0);
}

bool LanesPanel::handleKey (const juce::KeyPress& key)
{
    auto& s = processor.session();
    const auto mods = key.getModifiers();
    const bool cmd = mods.isCommandDown();
    const int code = (int) juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) key.getKeyCode());
    const auto sel = overlay.selection();
    if (! cmd && (code == 'S' || code == 's'))                      { splitAtSelectionOrPlayhead(); return true; }
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) { deleteSelection(); return sel.has_value(); }
    if (! cmd && (code == 'F' || code == 'f'))                      { fillWithNewTake(); return sel.has_value(); }
    if (! cmd && (code == 'G' || code == 'g'))
    {
        const auto v = s.editView();
        s.setGrid (v.gridBeats > 0.0 ? 0.0 : 1.0);
        s.notify (v.gridBeats > 0.0 ? "Snap off: drag freely" : "Snap to beats");
        return true;
    }
    if (! cmd && code == 'T')
    {
        const bool on = ! overlay.getStretchEdges();
        setStretchEdges (on);
        s.notify (on ? "Edges stretch (Alt+drag trims)" : "Edges trim (Alt+drag stretches)");
        return true;
    }
    if (key == juce::KeyPress::escapeKey && sel)                    { overlay.setSelection (std::nullopt); return true; }
    if (cmd && code == 'A')                                         { overlay.selectAll(); return true; }
    if (cmd && code == 'C' && sel)                                  { s.editCopy (sel->first, sel->second); s.notify ("Copied " + rangeText (sel->first, sel->second)); return true; }
    if (cmd && code == 'X' && sel)                                  { s.editCut (sel->first, sel->second); overlay.setSelection (std::nullopt); return true; }
    if (cmd && code == 'V')                                         { s.editPaste (overlay.snapBeat (playheadBeat(), false)); return true; }
    if (cmd && code == 'D' && sel)                                  { s.editDuplicate (sel->first, sel->second); return true; }
    return false;
}

// ---------------------------------------------------------------------------

void LanesPanel::paint (juce::Graphics& g)
{
    auto& s = processor.session();
    const auto st = s.getState();

    // ---- context header ----
    {
        auto r = ctxHeader;
        auto row1 = r.removeFromTop (18);
        paintHeading (g, "Your track", row1.withTrimmedRight (24));
        r.removeFromTop (2);
        r.removeFromTop (22);
        // (BPM and key are the chips.)
        const auto analysis = s.contextAnalysis();

        auto row3 = r.removeFromTop (16);
        if (tempoMismatch)
        {
            g.setColour (colours::warn);
            g.setFont (font (type::meta, Weight::Medium));
            g.drawText ("Captured at " + bpmString (st.context->tempo.bpm) + ", DAW now " + bpmString (*s.hostBpm()),
                        row3, juce::Justification::centredLeft, true);
        }
        else if (analysis)
        {
            // Compact analysis readout: low / mid / high as 3-step meters, then character.
            const Level levels[] = { analysis->lowEnd, analysis->mids, analysis->highs };
            const char* names[] = { "L", "M", "H" };
            auto meters = row3.removeFromLeft (72);
            for (int i = 0; i < 3; ++i)
            {
                auto cell = meters.removeFromLeft (24);
                g.setColour (colours::faint);
                g.setFont (font (10.5f, Weight::SemiBold));
                g.drawText (names[i], cell.removeFromLeft (9), juce::Justification::centredLeft);
                const int lit = levels[i] == Level::High ? 3 : levels[i] == Level::Medium ? 2 : 1;
                for (int k = 0; k < 3; ++k)
                {
                    g.setColour (k < lit ? (levels[i] == Level::High && k == 2 ? colours::warn : colours::dim) : colours::border);
                    g.fillRect ((float) cell.getX() + (float) k * 4.0f, (float) cell.getCentreY() + 3.0f - (float) (k + 1) * 2.5f, 3.0f, (float) (k + 1) * 2.5f);
                }
            }
            g.setColour (colours::faint);
            g.setFont (font (type::meta));
            g.drawText (toString (analysis->stereo) + ", " + toString (analysis->tonal).toLowerCase(), row3, juce::Justification::centredLeft, true);
        }
        else if (st.context && ! s.hasContext())
        {
            g.setColour (colours::warn);
            g.setFont (font (type::meta));
            g.drawText ("Capture unavailable", row3, juce::Justification::centredLeft);
        }
        else
        {
            g.setColour (colours::faint);
            g.setFont (font (type::meta));
            const auto src = s.tempoSource();
            g.drawText (src == SessionController::TempoSource::Host ? "Tempo from the DAW"
                        : src == SessionController::TempoSource::Set ? "Your tempo (not the DAW's)" : "No tempo from the DAW",
                        row3, juce::Justification::centredLeft);
        }
    }

    // ---- output header ----
    {
        auto r = outHeader;
        paintHeading (g, "Sound", r.removeFromTop (18).withTrimmedRight (24 + takesButton.getWidth()));
        r.removeFromTop (2);
        const auto src = s.outputSource();
        g.setColour (src != nullptr ? colours::text : colours::faint);
        g.setFont (font (type::body + 0.5f, Weight::Medium));
        const auto name = src != nullptr ? src->displayName : juce::String ("Nothing yet");
        g.drawText (fitText (name, font (type::body + 0.5f, Weight::Medium), (float) r.getWidth()), r.removeFromTop (20), juce::Justification::centredLeft, false);
        g.setColour (colours::faint);
        g.setFont (font (type::meta));
        g.drawText (fitText (outputDetail(), font (type::meta), (float) r.getWidth()), r.removeFromTop (16), juce::Justification::centredLeft, false);
    }

    // ---- fit strip ----
    paintHeading (g, "Fit", fitHeader);
    g.setColour (pitch.isEnabled() ? colours::dim : colours::disabled);
    g.setFont (numberFont (type::label));
    const double pv = pitch.getValue();
    g.drawText ((pv > 0.0 ? "+" : "") + juce::String (pv, 1) + " st", pitchValue, juce::Justification::centredLeft);
    g.setColour (colours::dim);
    g.setFont (labelFont());
    g.drawText ("Pitch", pitchLabel, juce::Justification::centredLeft);
    const auto fit = s.fitStatus();
    g.setColour (fit.error.isNotEmpty() ? colours::warn : colours::dim);
    g.setFont (font (type::label));
    auto info = fitInfo;
    if (fit.rendering)
        drawProgress (g, info.removeFromRight (80).withSizeKeepingCentre (80, 3).toFloat(), fit.progress);
    g.drawText (fitText (fitSummary(), font (type::label), (float) info.getWidth() - 8.0f), info, juce::Justification::centredLeft, false);

    // separators
    paintSeparator (g, outHeader.getBottom() + 6, 0, getWidth());
}

void LanesPanel::layoutLaneHeader (juce::Rectangle<int> area, bool isContext)
{
    auto r = area;
    auto row1 = r.removeFromTop (18);
    (isContext ? ctxMenu : outMenu).setBounds (row1.removeFromRight (22).withSizeKeepingCentre (22, 18));
    if (! isContext)
    {
        const int w = juce::GlyphArrangement::getStringWidthInt (font (type::label, Weight::Medium), takesButton.getButtonText()) + 18;
        takesButton.setBounds (row1.removeFromRight (w).withSizeKeepingCentre (w, 20));
    }
    if (isContext)
    {
        // Row 2: [155 v] BPM [C# minor v]  - the values are the controls.
        auto row2 = r.withTrimmedTop (2).removeFromTop (24);
        bpmChip.setBounds (row2.getX() - 5, row2.getY(), bpmChip.idealWidth(), 24);
        keyChip.setBounds (bpmChip.getRight() + 4, row2.getY(), juce::jmin (keyChip.idealWidth(), row2.getRight() - bpmChip.getRight() - 4), 24);
    }

    // Bottom up: [M][S] gain, then the lane's main action.
    auto mix = area.removeFromBottom (20);
    (isContext ? ctxMute : outMute).setBounds (mix.removeFromLeft (20));
    mix.removeFromLeft (3);
    (isContext ? ctxSolo : outSolo).setBounds (mix.removeFromLeft (20));
    mix.removeFromLeft (8);
    (isContext ? ctxGain : outGain).setBounds (mix);
    area.removeFromBottom (6);
    auto& main = isContext ? captureButton : loadButton;
    main.setBounds (area.removeFromBottom (height::control));
    main.setVisible (area.getHeight() >= 56);
}

void LanesPanel::resized()
{
    auto r = getLocalBounds().withTrimmedLeft (space::l).withTrimmedRight (space::l);
    auto rulerRow = r.removeFromTop (24);
    {
        auto tools = rulerRow.withWidth (headerWidth - space::m).withSizeKeepingCentre (headerWidth - space::m, 22).withX (rulerRow.getX());
        const int modeW = juce::jmin (112, tools.getWidth() / 2);
        edgeMode.setBounds (tools.removeFromRight (modeW));
        tools.removeFromRight (6);
        snap.setBounds (tools);
    }
    rulerArea = rulerRow.withTrimmedLeft (headerWidth);
    ruler.setBounds (rulerArea);

    auto fitRow = r.removeFromBottom (32);
    r.removeFromBottom (4);
    const int laneH = (r.getHeight() - space::s) / 2;
    auto ctxRow = r.removeFromTop (laneH);
    r.removeFromTop (space::s);
    auto outRow = r.removeFromTop (laneH);

    ctxHeader = ctxRow.removeFromLeft (headerWidth).withTrimmedRight (space::m);
    contextLane.setBounds (ctxRow);
    outHeader = outRow.removeFromLeft (headerWidth).withTrimmedRight (space::m);
    outputLane.setBounds (outRow);
    if (verdict.isVisible())
        layoutVerdict();
    overlay.setBounds (outputLane.getLocalBounds());
    layoutLaneHeader (ctxHeader, true);
    layoutLaneHeader (outHeader, false);

    if (tempoMismatch)
    {
        auto b = contextLane.getBounds().removeFromTop (28).reduced (6, 4);
        recaptureButton.setBounds (b.removeFromRight (100));
        b.removeFromRight (6);
        retargetButton.setBounds (b.removeFromRight (124));
    }

    fitHeader = fitRow.removeFromLeft (headerWidth).withTrimmedRight (space::m);
    auto controls = fitRow.withSizeKeepingCentre (fitRow.getWidth(), height::control);
    fitMode.setBounds (controls.removeFromLeft (juce::jmax (250, fitMode.idealWidth())));
    controls.removeFromLeft (space::s);
    algorithm.setBounds (controls.removeFromLeft (112));
    controls.removeFromLeft (space::l);
    pitchLabel = controls.removeFromLeft (38);
    pitch.setBounds (controls.removeFromLeft (110));
    pitchValue = controls.removeFromLeft (52).withTrimmedLeft (6);
    controls.removeFromLeft (space::s);
    fitInfo = controls;
}
} // namespace context::ui
