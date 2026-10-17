#include "LanesPanel.h"
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

LanesPanel::LanesPanel (ContextAudioProcessor& p) : processor (p)
{
    addAndMakeVisible (ruler);
    addAndMakeVisible (contextLane);
    addAndMakeVisible (outputLane);

    contextLane.setTooltip ("Your track: the 8 bars captured from the DAW");
    contextLane.onRightClick = [this] (const juce::MouseEvent&) { showContextMenu(); };
    contextLane.onClick = [this]
    {
        if (! processor.session().hasContext() && ! processor.session().captureStatus().busy())
            processor.session().startCapture();
    };

    outputLane.setTooltip ("Generated or dropped-in sound, fitted to the bar length. Drag it into your DAW.");
    outputLane.onRightClick = [this] (const juce::MouseEvent&) { showOutputMenu(); };
    outputLane.onFileDropped = [this] (const juce::File& f) { processor.session().importFile (f); };
    outputLane.onDragOut = [this]() -> juce::File
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

    manualTempo.setEditable (true, true, false);
    manualTempo.setFont (numberFont (15.0f, Weight::SemiBold));
    manualTempo.setColour (juce::Label::textColourId, colours::text);
    manualTempo.setColour (juce::Label::backgroundWhenEditingColourId, colours::well);
    manualTempo.setTooltip ("No tempo from the DAW: click to set the tempo used for bars and fitting");
    manualTempo.onTextChange = [this]
    {
        const double v = manualTempo.getText().retainCharacters ("0123456789.").getDoubleValue();
        if (musical::isValidBpm (v))
            processor.session().setManualTempo (v);
        refresh();
    };
    addChildComponent (manualTempo);

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
    juce::String d = src->origin == AudioAsset::Origin::Generated ? "Generated" : "Imported";
    d << glyph (" \xc2\xb7 ") << juce::String (src->seconds(), src->seconds() < 10.0 ? 2 : 1) << " s";
    const auto fit = s.fitStatus();
    if (fit.hasReport)
    {
        d << glyph (" \xc2\xb7 ") << juce::String (fit.report.sourceBars, 2) << " bars";
        if (fit.report.sourceBpm)
            d << glyph (" \xe2\x89\x88") << juce::roundToInt (*fit.report.sourceBpm) << " BPM";
    }
    return d;
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
    const auto& r = fit.report;
    const auto arrow = glyph (" \xe2\x86\x92 ");
    juce::String t;
    t << juce::String (r.sourceBars, 2) << arrow << r.targetBars << " bars";
    switch (r.modeUsed)
    {
        case FitMode::Stretch: t << glyph (" \xc2\xb7 Fit ") << (r.stretchPercent >= 0 ? "+" : "") << juce::String (r.stretchPercent, 1) << "%"; break;
        case FitMode::Repitch: t << glyph (" \xc2\xb7 Repitch ") << (r.pitchSemitones >= 0 ? "+" : "") << juce::String (r.pitchSemitones, 1) << " st"; break;
        case FitMode::Loop:    t << glyph (" \xc2\xb7 Loop x") << r.loopRepeats; if (r.engineUsed) t << " (" << (r.stretchPercent >= 0 ? "+" : "") << juce::String (r.stretchPercent, 1) << "%)"; break;
        case FitMode::Trim:    t << glyph (" \xc2\xb7 ") << (r.sourceBars < r.targetBars ? "Padded" : "Trimmed"); break;
        case FitMode::Auto:    break;
    }
    if (r.engineUsed && r.modeUsed != FitMode::Repitch)
        t << glyph (" \xc2\xb7 ") << (s.getState().fit.algorithm == stretch::Algorithm::Auto ? "Auto: " : "") << stretch::toLabel (r.algorithmUsed);
    if (std::abs (r.pitchSemitones) > 0.05 && r.modeUsed != FitMode::Repitch)
        t << glyph (" \xc2\xb7 ") << (r.pitchSemitones > 0 ? "+" : "") << juce::String (r.pitchSemitones, 1) << " st";
    if (s.getState().fit.mode == FitMode::Auto)
        t << "  (Auto: " << toLabel (r.modeUsed) << ")";
    return t;
}

void LanesPanel::refresh()
{
    auto& s = processor.session();
    const auto st = s.getState();
    timeline = computeTimeline();
    ruler.setTimeline (timeline);
    contextLane.setTimeline (timeline);
    outputLane.setTimeline (timeline);

    const auto ctx = s.contextLane();
    juce::int64 ctxLen = 0;
    if (ctx.hasAudio && ctx.sampleRate > 0)
        ctxLen = (juce::int64) std::llround ((double) ctx.lengthSamples * timeline.sampleRate / ctx.sampleRate);
    contextLane.setPeaks (ctx.peaks, ctxLen);
    contextLane.setMissing (ctx.missing ? ctx.message : juce::String());
    const auto cap = s.captureStatus();
    if (cap.armed())
        contextLane.setEmptyText ("Press play in your DAW", "Recording starts on the next bar line and stops after exactly 8 bars");
    else
        contextLane.setEmptyText ("Capture your track", "Click Capture, then play your DAW. Context records exactly 8 bars.");
    contextLane.setBusy (cap.processing && ! cap.recording(), "Analysing the capture...");

    const auto out = s.outputLane();
    outputLane.setPeaks (out.peaks, out.hasAudio ? out.lengthSamples : 0);
    outputLane.setMissing (out.missing ? out.message : juce::String());
    outputLane.setEmptyText ("Drop audio here", "or generate something above");

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

    const bool manual = s.tempoSource() == SessionController::TempoSource::Manual;
    manualTempo.setVisible (manual);
    if (manual && ! manualTempo.isBeingEdited())
        manualTempo.setText (bpmString (st.manualBpm), juce::dontSendNotification);

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
    ruler.setPlayhead (pos, playing);
    contextLane.setPlayhead (pos, playing && contextLane.hasAudio());
    outputLane.setPlayhead (pos, playing && outputLane.hasAudio());

    const auto cap = s.captureStatus();
    contextLane.setCaptureProgress (cap.progress, cap.recording());
    if (cap.recording() != wasRecording || (cap.recording() && cap.barsDone != lastBarsShown))
    {
        wasRecording = cap.recording();
        lastBarsShown = cap.barsDone;
        refresh();
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
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&ctxMenu), [safe] (int r)
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

void LanesPanel::showOutputMenu()
{
    auto& s = processor.session();
    const auto src = s.outputSource();
    juce::PopupMenu m;
    m.addItem (1, "Load sample...");
    m.addItem (2, "Restore generated version", s.canRestoreGenerated());
    m.addItem (5, "Reset fit settings");
    m.addSeparator();
    m.addItem (3, "Show original file", src != nullptr && src->sourceFile.existsAsFile());
    m.addItem (4, "Clear", src != nullptr || s.outputLane().missing);
    juce::Component::SafePointer<LanesPanel> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&outMenu), [safe, src] (int r)
    {
        if (safe == nullptr) return;
        auto& ss = safe->processor.session();
        if (r == 1) safe->openSampleChooser();
        if (r == 2) ss.restoreGenerated();
        if (r == 3 && src != nullptr) src->sourceFile.revealToUser();
        if (r == 4) ss.clearOutput();
        if (r == 5) ss.resetFit();
    });
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
        paintHeading (g, "Track Context", row1.withTrimmedRight (24));
        r.removeFromTop (2);
        auto row2 = r.removeFromTop (22);
        const auto tempo = s.sessionTempo();
        const auto src = s.tempoSource();
        if (src != SessionController::TempoSource::Manual)
        {
            g.setColour (colours::text);
            g.setFont (numberFont (15.0f, Weight::SemiBold));
            g.drawText (bpmString (tempo.bpm), row2, juce::Justification::centredLeft);
        }
        const int bpmW = src == SessionController::TempoSource::Manual ? manualTempo.getWidth() - 6
                                                                        : juce::GlyphArrangement::getStringWidthInt (numberFont (15.0f, Weight::SemiBold), bpmString (tempo.bpm));
        auto rest = row2.withTrimmedLeft (bpmW + 4);
        g.setColour (colours::faint);
        g.setFont (font (11.0f, Weight::Medium));
        const auto bpmLabel = juce::String ("BPM") + (src == SessionController::TempoSource::Manual ? " (set)" : "");
        g.drawText (bpmLabel, rest.removeFromLeft (juce::GlyphArrangement::getStringWidthInt (font (11.0f, Weight::Medium), bpmLabel) + 8),
                    juce::Justification::centredLeft);
        const auto analysis = s.contextAnalysis();
        juce::String key = analysis && analysis->keyKnown ? analysis->keyName() + (analysis->minor ? " minor" : " major") : juce::String();
        if (key.isEmpty() && st.context && st.context->keyDisplay.isNotEmpty())
            key = st.context->keyDisplay;
        if (key.isNotEmpty())
        {
            g.setColour (colours::text);
            g.setFont (font (13.0f, Weight::Medium));
            g.drawText (key, rest, juce::Justification::centredLeft, true);
        }

        auto row3 = r.removeFromTop (16);
        if (tempoMismatch)
        {
            g.setColour (colours::warn);
            g.setFont (font (11.0f, Weight::Medium));
            g.drawText ("Captured at " + bpmString (st.context->tempo.bpm) + glyph (" \xc2\xb7 host now ") + bpmString (*s.hostBpm()),
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
                g.setFont (font (9.5f, Weight::SemiBold));
                g.drawText (names[i], cell.removeFromLeft (9), juce::Justification::centredLeft);
                const int lit = levels[i] == Level::High ? 3 : levels[i] == Level::Medium ? 2 : 1;
                for (int k = 0; k < 3; ++k)
                {
                    g.setColour (k < lit ? (levels[i] == Level::High && k == 2 ? colours::warn : colours::dim) : colours::border);
                    g.fillRect ((float) cell.getX() + (float) k * 4.0f, (float) cell.getCentreY() + 3.0f - (float) (k + 1) * 2.5f, 3.0f, (float) (k + 1) * 2.5f);
                }
            }
            g.setColour (colours::faint);
            g.setFont (font (10.5f));
            g.drawText (toString (analysis->stereo) + glyph (" \xc2\xb7 ") + toString (analysis->tonal), row3, juce::Justification::centredLeft, true);
        }
        else if (st.context && ! s.hasContext())
        {
            g.setColour (colours::warn);
            g.setFont (font (11.0f));
            g.drawText ("Capture unavailable", row3, juce::Justification::centredLeft);
        }
        else
        {
            g.setColour (colours::faint);
            g.setFont (font (10.5f));
            g.drawText (src == SessionController::TempoSource::Host ? "Tempo from the DAW" : "No tempo from the DAW", row3, juce::Justification::centredLeft);
        }
    }

    // ---- output header ----
    {
        auto r = outHeader;
        paintHeading (g, "AI / Sample", r.removeFromTop (18).withTrimmedRight (24));
        r.removeFromTop (2);
        const auto src = s.outputSource();
        g.setColour (src != nullptr ? colours::text : colours::faint);
        g.setFont (font (12.5f, Weight::Medium));
        const auto name = src != nullptr ? src->displayName : juce::String ("Nothing loaded");
        g.drawText (fitText (name, font (12.5f, Weight::Medium), (float) r.getWidth()), r.removeFromTop (20), juce::Justification::centredLeft, false);
        g.setColour (colours::faint);
        g.setFont (font (10.5f));
        g.drawText (fitText (outputDetail(), font (10.5f), (float) r.getWidth()), r.removeFromTop (16), juce::Justification::centredLeft, false);
    }

    // ---- fit strip ----
    paintHeading (g, "Fit", fitHeader);
    g.setColour (pitch.isEnabled() ? colours::dim : colours::disabled);
    g.setFont (numberFont (11.5f));
    const double pv = pitch.getValue();
    g.drawText ((pv > 0.0 ? "+" : "") + juce::String (pv, 1) + " st", pitchValue, juce::Justification::centredLeft);
    g.setColour (colours::faint);
    g.setFont (labelFont());
    g.drawText ("PITCH", pitchLabel, juce::Justification::centredLeft);
    const auto fit = s.fitStatus();
    g.setColour (fit.error.isNotEmpty() ? colours::warn : colours::dim);
    g.setFont (font (11.5f));
    auto info = fitInfo;
    if (fit.rendering)
        drawProgress (g, info.removeFromRight (80).withSizeKeepingCentre (80, 3).toFloat(), fit.progress);
    g.drawText (fitText (fitSummary(), font (11.5f), (float) info.getWidth() - 8.0f), info, juce::Justification::centredLeft, false);

    // separators
    paintSeparator (g, outHeader.getBottom() + 6, 0, getWidth());
}

void LanesPanel::layoutLaneHeader (juce::Rectangle<int> area, bool isContext)
{
    auto r = area;
    auto row1 = r.removeFromTop (18);
    (isContext ? ctxMenu : outMenu).setBounds (row1.removeFromRight (22).withSizeKeepingCentre (22, 18));
    if (isContext)
        manualTempo.setBounds (r.getX() - 3, r.getY() + 2,
                               juce::GlyphArrangement::getStringWidthInt (numberFont (15.0f, Weight::SemiBold), manualTempo.getText()) + 12, 22);

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
    rulerArea = r.removeFromTop (20).withTrimmedLeft (headerWidth);
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
