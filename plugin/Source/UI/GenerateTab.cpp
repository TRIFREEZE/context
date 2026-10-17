#include "GenerateTab.h"
#include "Theme.h"
#include "PluginProcessor.h"
#include "Utils/Redaction.h"

namespace context::ui
{
namespace
{
    juce::String formatSeconds (double s)
    {
        if (s < 60.0) return juce::String (s, s < 10.0 ? 2 : 1) + " s";
        return juce::String ((int) s / 60) + ":" + juce::String ((int) s % 60).paddedLeft ('0', 2);
    }

    juce::Colour levelColour (Level l)
    {
        return l == Level::High ? colours::warn : l == Level::Low ? colours::dim : colours::text;
    }
}

GenerateTab::GenerateTab (ContextAudioProcessor& p) : processor (p)
{
    // ---- Prompt ----
    prompt.setMultiLine (true, true);
    prompt.setReturnKeyStartsNewLine (false);
    prompt.setScrollbarsShown (true);
    prompt.setFont (ContextLookAndFeel::font (16.0f));
    prompt.setIndents (10, 8);
    prompt.setInputRestrictions (1000);
    prompt.setTextToShowWhenEmpty ("Describe the sound: \"dark distorted female vocal chop\"", colours::faint);
    prompt.onTextChange = [this] { pushUiState(); updatePromptPreview(); };
    prompt.onReturnKey = [this] { startGeneration(); };
    addAndMakeVisible (prompt);

    for (size_t i = 0; i < typeButtons.size(); ++i)
    {
        auto& b = typeButtons[i];
        b.setButtonText (toLabel (allGenerationTypes[i]));
        b.setClickingTogglesState (false);
        styleQuiet (b);
        b.onClick = [this, i]
        {
            auto s = processor.getUiState();
            s.type = allGenerationTypes[i];
            processor.setUiState (s);
            updateTypeButtons();
            updatePromptPreview();
        };
        addAndMakeVisible (b);
    }

    useContext.setTooltip ("Use the analyzed BPM, key and mix balance to improve the request. Analysis stays on this computer.");
    useContext.onClick = [this] { pushUiState(); updatePromptPreview(); };
    sendAudio.setTooltip ("Upload a short snapshot of your track to the generation bridge (only if the bridge supports it and you allowed it in Settings).");
    sendAudio.onClick = [this] { pushUiState(); };
    expand.setTooltip ("Add context hints to your prompt. Turn off to send your prompt exactly as typed.");
    expand.onClick = [this]
    {
        const bool on = expand.getToggleState();
        processor.services().settings().update ([on] (Settings& s) { s.promptExpansion = on; });
        updatePromptPreview();
    };
    addAndMakeVisible (useContext);
    addAndMakeVisible (sendAudio);
    addAndMakeVisible (expand);

    enhancedPrompt.setMultiLine (true, true);
    enhancedPrompt.setReadOnly (true);
    enhancedPrompt.setCaretVisible (false);
    enhancedPrompt.setScrollbarsShown (true);
    enhancedPrompt.setFont (ContextLookAndFeel::font (12.5f));
    enhancedPrompt.setColour (juce::TextEditor::textColourId, colours::dim);
    enhancedPrompt.setColour (juce::TextEditor::backgroundColourId, colours::panel);
    enhancedPrompt.setColour (juce::TextEditor::outlineColourId, colours::panel);
    enhancedPrompt.setIndents (2, 2);
    addAndMakeVisible (enhancedPrompt);

    stylePrimary (generateButton);
    generateButton.onClick = [this] { startGeneration(); };
    generateButton.addShortcut (juce::KeyPress (juce::KeyPress::returnKey, juce::ModifierKeys::ctrlModifier, 0));
    addAndMakeVisible (generateButton);

    cancelButton.onClick = [this] { processor.generation().cancelActive(); };
    addAndMakeVisible (cancelButton);

    statusLabel.setFont (ContextLookAndFeel::font (13.0f));
    statusLabel.setColour (juce::Label::textColourId, colours::dim);
    addAndMakeVisible (statusLabel);

    // ---- Analysis ----
    analyzeButton.setTooltip ("Analyze the last few seconds of audio playing through Context. Nothing is uploaded.");
    analyzeButton.onClick = [this] { processor.analyzeTrack(); refresh(); };
    addAndMakeVisible (analyzeButton);
    analysisMessage.setFont (ContextLookAndFeel::font (12.5f));
    analysisMessage.setColour (juce::Label::textColourId, colours::dim);
    analysisMessage.setJustificationType (juce::Justification::topLeft);
    analysisMessage.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (analysisMessage);

    // ---- Result ----
    waveform.onDragStarted = [this]
    {
        if (auto r = currentResult())
            processor.services().history().setPinned (r->generationId);   // now referenced by a DAW project
    };
    waveform.onClicked = [this] { if (auto r = currentResult()) processor.previewResult (*r); };
    addAndMakeVisible (waveform);

    resultTitle.setFont (ContextLookAndFeel::font (15.0f, true));
    resultMeta.setFont (ContextLookAndFeel::font (12.5f));
    resultMeta.setColour (juce::Label::textColourId, colours::dim);
    addAndMakeVisible (resultTitle);
    addAndMakeVisible (resultMeta);

    playButton.onClick = [this] { if (auto r = currentResult()) processor.previewResult (*r); };
    stopButton.onClick = [this] { processor.preview().stop(); refresh(); };
    favoriteButton.onClick = [this]
    {
        if (auto r = currentResult())
            processor.services().history().setFavorite (r->generationId, ! r->favorite);
    };
    regenerateButton.setTooltip ("Generate again with the same prompt and context");
    regenerateButton.onClick = [this]
    {
        if (auto r = currentResult())
        {
            auto res = processor.regenerate (*r);
            if (! res.ok) showError (res.error); else clearError();
            refresh();
        }
    };
    variationButton.onClick = [this]
    {
        if (auto r = currentResult())
        {
            auto res = processor.variation (*r);
            if (! res.ok) showError (res.error); else clearError();
            refresh();
        }
    };
    revealButton.setTooltip ("Open the folder containing this file");
    revealButton.onClick = [this]
    {
        if (auto r = currentResult())
        {
            processor.services().history().setPinned (r->generationId);
            r->localFile.revealToUser();
        }
    };
    copyPathButton.setTooltip ("Copy the file path (paste it into your DAW's file browser)");
    copyPathButton.onClick = [this]
    {
        if (auto r = currentResult())
        {
            processor.services().history().setPinned (r->generationId);
            juce::SystemClipboard::copyTextToClipboard (r->localFile.getFullPathName());
            statusLabel.setText ("File path copied.", juce::dontSendNotification);
        }
    };
    saveAsButton.setTooltip ("Save a copy anywhere (copies are never touched by Context's cache cleanup)");
    saveAsButton.onClick = [this] { saveAs(); };

    for (auto* b : { &playButton, &stopButton, &favoriteButton, &regenerateButton, &variationButton, &revealButton, &copyPathButton, &saveAsButton })
    {
        styleQuiet (*b);
        addAndMakeVisible (*b);
    }

    // ---- Errors ----
    errorLabel.setFont (ContextLookAndFeel::font (13.5f));
    errorLabel.setColour (juce::Label::textColourId, colours::text);
    addChildComponent (errorLabel);
    detailsButton.onClick = [this] { showDetails(); };
    dismissButton.onClick = [this] { clearError(); };
    styleQuiet (detailsButton);
    styleQuiet (dismissButton);
    addChildComponent (detailsButton);
    addChildComponent (dismissButton);

    const auto s = processor.getUiState();
    prompt.setText (s.prompt, juce::dontSendNotification);
    refresh();
}

GenerateTab::~GenerateTab() = default;

void GenerateTab::focusPrompt()
{
    if (isShowing())
        prompt.grabKeyboardFocus();
}

std::optional<GenerationResult> GenerateTab::currentResult() const
{
    return processor.getSelectedResult();
}

void GenerateTab::pushUiState()
{
    auto s = processor.getUiState();
    s.prompt = prompt.getText();
    s.useTrackContext = useContext.getToggleState();
    s.sendAudioContext = sendAudio.getToggleState();
    processor.setUiState (s);
}

void GenerateTab::updateTypeButtons()
{
    const auto t = processor.getUiState().type;
    const auto caps = processor.generation().getCapabilities();
    for (size_t i = 0; i < typeButtons.size(); ++i)
    {
        const auto type = allGenerationTypes[i];
        typeButtons[i].setToggleState (type == t, juce::dontSendNotification);
        const bool supported = caps.types.empty() || caps.supports (type);
        typeButtons[i].setEnabled (supported);
        typeButtons[i].setTooltip (supported ? juce::String() : "Not supported by the current generation provider");
    }
}

void GenerateTab::updatePromptPreview()
{
    const auto s = processor.getUiState();
    const auto out = processor.previewPrompt (s);
    juce::String text;
    if (s.prompt.trim().isEmpty())
        text = "Context-enhanced prompt will appear here.";
    else if (! processor.services().settings().get().promptExpansion)
        text = "Sent as typed (prompt enhancement off)" + (out.bpm || out.key.isNotEmpty()
                   ? juce::String (" + ") + (out.bpm ? juce::String (juce::roundToInt (*out.bpm)) + " BPM " : juce::String()) + out.key + " " + out.mode
                   : juce::String());
    else
        text = out.expandedPrompt;
    enhancedPrompt.setText (text, juce::dontSendNotification);
}

void GenerateTab::startGeneration()
{
    pushUiState();
    if (processor.getUiState().prompt.trim().isEmpty())
    {
        statusLabel.setText ("Type what sound you want first.", juce::dontSendNotification);
        prompt.grabKeyboardFocus();
        return;
    }
    const auto r = processor.generate();
    if (! r.ok)
        showError (r.error);
    else
        clearError();
    refresh();
}

void GenerateTab::showError (const ProviderError& e)
{
    lastError = e;
    errorVisible = true;
    errorLabel.setText (e.userMessage.isNotEmpty() ? e.userMessage : ProviderError::defaultUserMessage (e.code), juce::dontSendNotification);
    errorLabel.setVisible (true);
    detailsButton.setVisible (true);
    dismissButton.setVisible (true);
    resized();
    repaint();
}

void GenerateTab::clearError()
{
    errorVisible = false;
    errorLabel.setVisible (false);
    detailsButton.setVisible (false);
    dismissButton.setVisible (false);
    resized();
    repaint();
}

void GenerateTab::showDetails()
{
    juce::String detail;
    detail << "Error code: " << toId (lastError.code) << "\n";
    if (lastError.httpStatus != 0)
        detail << "HTTP status: " << lastError.httpStatus << "\n";
    detail << "\n" << redaction::redact (lastError.technicalDetail.isNotEmpty() ? lastError.technicalDetail : juce::String ("(no further detail)"))
           << "\n\nUse Settings > Diagnostics > Copy Diagnostic Info to share full details.";
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::WarningIcon)
                                      .withTitle ("Technical details")
                                      .withMessage (detail)
                                      .withButton ("Copy")
                                      .withButton ("Close")
                                      .withAssociatedComponent (this),
                                  [detail] (int result) { if (result == 1) juce::SystemClipboard::copyTextToClipboard (detail); });
}

void GenerateTab::saveAs()
{
    auto r = currentResult();
    if (! r || ! r->localFile.existsAsFile())
        return;
    const auto src = r->localFile;
    chooser = std::make_unique<juce::FileChooser> ("Save a copy of this sound",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (src.getFileName()),
                                                   "*" + src.getFileExtension());
    juce::Component::SafePointer<GenerateTab> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, src] (const juce::FileChooser& fc)
    {
        const auto dest = fc.getResult();
        if (dest == juce::File() || safe == nullptr)
            return;
        const auto target = dest.hasFileExtension (src.getFileExtension()) ? dest : dest.withFileExtension (src.getFileExtension());
        const bool ok = target != src && src.copyFileTo (target);
        safe->statusLabel.setText (ok ? "Saved a copy to " + target.getFullPathName() : "Couldn't save the copy.", juce::dontSendNotification);
    });
}

// ---------------------------------------------------------------------------

void GenerateTab::refresh()
{
    const auto ui = processor.getUiState();
    const auto settings = processor.services().settings().get();
    const auto caps = processor.generation().getCapabilities();
    const auto job = processor.generation().getCurrentJob();
    const bool licensed = processor.isLicensed();
    busy = processor.generation().isBusy();

    if (! prompt.hasKeyboardFocus (true) && prompt.getText() != ui.prompt)
        prompt.setText (ui.prompt, juce::dontSendNotification);

    // Context toggles
    const auto analysis = processor.analysis().getLatest();
    useContext.setToggleState (ui.useTrackContext, juce::dontSendNotification);
    const bool audioCtxPossible = settings.allowSendAudioContext && caps.has (Feature::AudioConditioning);
    sendAudio.setEnabled (audioCtxPossible);
    sendAudio.setToggleState (ui.sendAudioContext && audioCtxPossible, juce::dontSendNotification);
    sendAudio.setTooltip (audioCtxPossible ? "Upload a short snapshot of your track to the generation bridge."
                          : ! settings.allowSendAudioContext ? "Off: enable \"Allow sending audio context\" in Settings first. Track audio stays local by default."
                          : "The current generation provider doesn't accept audio context.");
    expand.setToggleState (settings.promptExpansion, juce::dontSendNotification);
    updateTypeButtons();
    updatePromptPreview();
    enhancedPrompt.setVisible (settings.showEnhancedPrompt);

    // Generate button / status
    generateButton.setEnabled (licensed && ! busy);
    generateButton.setTooltip (licensed ? "Generate (Enter)" : "Activate Context in Settings to generate.");
    cancelButton.setVisible (busy);

    juce::String status;
    if (! licensed)
        status = "Activate Context to generate (Settings > License).";
    else if (busy)
        status = job.statusText.isNotEmpty() ? job.statusText : toString (job.state);
    else if (job.state == JobState::Complete)
        status = "Done. Preview it, then drag it into your DAW.";
    else if (job.state == JobState::Cancelled)
        status = "Cancelled.";
    else if (job.state == JobState::Failed)
        status = "Generation failed.";
    else
        status = "Ready.";
    if (busy && processor.generation().numQueued() > 0)
        status << "  (" << processor.generation().numQueued() << " queued)";
    statusLabel.setText (status, juce::dontSendNotification);

    if (job.state == JobState::Failed && job.error.isError() && job.localJobId != lastFailedJobShown)
    {
        lastFailedJobShown = job.localJobId;
        showError (job.error);
    }

    // Analysis
    const auto aState = processor.analysis().getState();
    analyzeButton.setEnabled (aState != AnalysisController::State::Analyzing);
    analyzeButton.setButtonText (aState == AnalysisController::State::Analyzing ? "Analyzing..." : "Analyze Track");
    juce::String amsg = processor.analysis().getMessage();
    if (amsg.isEmpty() && ! analysis)
        amsg = "Play your track through Context, then click Analyze Track.";
    if (aState == AnalysisController::State::Analyzing)
        amsg = {};
    analysisMessage.setText (amsg, juce::dontSendNotification);

    // Result
    const auto result = currentResult();
    const bool hasResult = result.has_value() && result->localFile.existsAsFile();
    waveform.setFile (result ? result->localFile : juce::File());
    resultTitle.setText (result ? result->localFile.getFileNameWithoutExtension() : juce::String ("No sound yet"), juce::dontSendNotification);
    if (result)
    {
        juce::String meta = toLabel (result->type) + "  " + glyph ("\xc2\xb7") + "  " + formatSeconds (result->durationSeconds);
        if (result->bpm) meta << "  " << glyph ("\xc2\xb7") << "  " << juce::roundToInt (*result->bpm) << " BPM";
        if (result->key.isNotEmpty()) meta << "  " << glyph ("\xc2\xb7") << "  " << result->key << " " << result->mode;
        meta << "  " << glyph ("\xc2\xb7") << "  \"" << result->prompt.substring (0, 60) << (result->prompt.length() > 60 ? "..." : "") << "\"";
        resultMeta.setText (meta, juce::dontSendNotification);
    }
    else
    {
        resultMeta.setText ("Type a prompt and click Generate.", juce::dontSendNotification);
    }

    const bool playing = processor.preview().isPlaying() && processor.preview().loadedFile() == (result ? result->localFile : juce::File());
    for (auto* b : { &playButton, &revealButton, &copyPathButton, &saveAsButton })
        b->setEnabled (hasResult);
    playButton.setButtonText (playing ? "Playing" : glyph ("\xe2\x96\xb6  Play"));
    stopButton.setEnabled (playing);
    favoriteButton.setEnabled (hasResult);
    favoriteButton.setButtonText (result && result->favorite ? glyph ("\xe2\x98\x85  Favorited") : glyph ("\xe2\x98\x86  Favorite"));
    favoriteButton.setColour (juce::TextButton::textColourOffId, result && result->favorite ? colours::favorite : colours::text);
    regenerateButton.setEnabled (result.has_value() && licensed && ! busy);
    const bool canVar = processor.canVariation();
    variationButton.setEnabled (result.has_value() && licensed && ! busy && canVar);
    variationButton.setTooltip (canVar ? "Create a variation of this sound" : "The current provider doesn't support variations");

    if (const auto err = processor.getPreviewError(); err.isNotEmpty())
        statusLabel.setText (err, juce::dontSendNotification);

    repaint();
}

void GenerateTab::tick()
{
    const auto job = processor.generation().getCurrentJob();
    const bool nowBusy = processor.generation().isBusy();
    const double newProgress = nowBusy ? job.progress : -2.0;
    if (nowBusy != busy || std::abs (newProgress - progress) > 1.0e-3 || nowBusy)
    {
        progress = newProgress;
        repaint (statusArea);
    }
    if (nowBusy != busy)
        refresh();

    auto& pv = processor.preview();
    const auto r = currentResult();
    const bool sameFile = r && pv.loadedFile() == r->localFile;
    waveform.setPlayhead (pv.positionSeconds(), sameFile && pv.isPlaying());
    const bool playing = sameFile && pv.isPlaying();
    if (playButton.getButtonText().startsWith ("Playing") != playing)
        refresh();
    repaint (analysisCard);
}

// ---------------------------------------------------------------------------

void GenerateTab::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    paintCard (g, leftCard.toFloat());
    paintCard (g, analysisCard.toFloat());
    paintCard (g, resultCard.toFloat());

    paintHeading (g, "Prompt", { leftCard.getX() + 14, leftCard.getY() + 8, 200, 18 });
    paintHeading (g, "Sound type", { leftCard.getX() + 14, typeButtons[0].getY() - 22, 200, 18 });
    paintHeading (g, "Track context", { analysisCard.getX() + 14, analysisCard.getY() + 8, 200, 18 });
    paintHeading (g, "Result", { resultCard.getX() + 14, resultCard.getY() + 8, 200, 18 });

    // Progress strip
    if (busy)
        drawProgress (g, statusArea.withTop (statusArea.getBottom() - 4).toFloat(), progress);

    // Analysis readout
    const auto a = processor.analysis().getLatest();
    const auto host = processor.getHostInfo();
    auto rows = analysisRows;
    auto row = [&] (const juce::String& name, const juce::String& value, juce::Colour c)
    {
        auto r = rows.removeFromTop (21);
        g.setFont (ContextLookAndFeel::font (13.0f));
        g.setColour (colours::dim);
        g.drawText (name, r.removeFromLeft (92), juce::Justification::centredLeft);
        g.setColour (c);
        g.setFont (ContextLookAndFeel::font (13.5f, true));
        g.drawText (value, r, juce::Justification::centredLeft);
    };

    juce::String bpm = "-";
    if (host.bpm)                        bpm = juce::String (*host.bpm, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " BPM (host)";
    else if (a && a->estimatedBpm)       bpm = "~" + juce::String (juce::roundToInt (*a->estimatedBpm)) + " BPM (estimated)";
    row ("Tempo", bpm, colours::text);
    if (a && a->valid)
    {
        row ("Key", a->keyKnown ? a->keyDisplay() + "  (" + juce::String (juce::roundToInt (a->keyConfidence * 100)) + "%)" : "Unknown", a->keyKnown ? colours::text : colours::dim);
        row ("Low End", toString (a->lowEnd), levelColour (a->lowEnd));
        row ("Mids", toString (a->mids), levelColour (a->mids));
        row ("Highs", toString (a->highs), levelColour (a->highs));
        row ("Stereo", toString (a->stereo), colours::text);
        row ("Transients", toString (a->transients), colours::text);
        row ("Tone", toString (a->tonal), colours::text);
    }
    else
    {
        for (auto* n : { "Key", "Low End", "Mids", "Highs", "Stereo", "Transients", "Tone" })
            row (n, "-", colours::faint);
    }

    // Capture meter
    const auto& cap = processor.capture();
    auto meter = juce::Rectangle<int> (analysisCard.getX() + 14, analysisCard.getBottom() - 22, analysisCard.getWidth() - 28, 14);
    g.setColour (colours::faint);
    g.setFont (ContextLookAndFeel::font (11.5f));
    g.drawText ("Listening buffer: " + juce::String (cap.capturedSeconds(), 1) + " / " + juce::String (cap.capacitySeconds(), 0) + " s",
                meter, juce::Justification::centredLeft);

    // Error banner
    if (errorVisible)
    {
        auto banner = errorLabel.getBounds().getUnion (dismissButton.getBounds()).expanded (8, 5).toFloat();
        g.setColour (colours::bad.withAlpha (0.14f));
        g.fillRoundedRectangle (banner, 6.0f);
        g.setColour (colours::bad.withAlpha (0.6f));
        g.drawRoundedRectangle (banner.reduced (0.5f), 6.0f, 1.0f);
    }
}

void GenerateTab::resized()
{
    auto area = getLocalBounds().reduced (14, 12);
    const int topH = juce::jlimit (300, 380, (int) (area.getHeight() * 0.56f));
    auto top = area.removeFromTop (topH);
    area.removeFromTop (10);

    if (errorVisible)
    {
        auto e = area.removeFromTop (34).reduced (8, 4);
        dismissButton.setBounds (e.removeFromRight (80));
        e.removeFromRight (6);
        detailsButton.setBounds (e.removeFromRight (80));
        e.removeFromRight (6);
        errorLabel.setBounds (e);
        area.removeFromTop (8);
    }
    resultCard = area;

    analysisCard = top.removeFromRight (juce::jmax (250, top.getWidth() * 34 / 100));
    top.removeFromRight (10);
    leftCard = top;

    // ---- left card ----
    auto l = leftCard.reduced (14, 10);
    l.removeFromTop (22);
    prompt.setBounds (l.removeFromTop (58));
    l.removeFromTop (28);
    for (int rowIdx = 0; rowIdx < 2; ++rowIdx)
    {
        auto row = l.removeFromTop (30);
        const int w = (row.getWidth() - 3 * 6) / 4;
        for (int c = 0; c < 4; ++c)
        {
            typeButtons[(size_t) (rowIdx * 4 + c)].setBounds (row.removeFromLeft (w));
            row.removeFromLeft (6);
        }
        l.removeFromTop (6);
    }
    l.removeFromTop (2);
    auto toggles = l.removeFromTop (24);
    const int tw = toggles.getWidth() / 3;
    useContext.setBounds (toggles.removeFromLeft (tw));
    sendAudio.setBounds (toggles.removeFromLeft (tw));
    expand.setBounds (toggles);
    l.removeFromTop (4);

    auto gen = l.removeFromBottom (44);
    statusArea = gen;
    generateButton.setBounds (gen.removeFromLeft (170).withTrimmedBottom (6));
    gen.removeFromLeft (8);
    if (cancelButton.isVisible())
    {
        cancelButton.setBounds (gen.removeFromLeft (80).withTrimmedBottom (6));
        gen.removeFromLeft (8);
    }
    statusLabel.setBounds (gen.withTrimmedBottom (6));
    l.removeFromBottom (6);
    enhancedPrompt.setBounds (l);

    // ---- analysis card ----
    auto a = analysisCard.reduced (14, 10);
    a.removeFromTop (22);
    analyzeButton.setBounds (a.removeFromTop (32));
    a.removeFromTop (8);
    analysisRows = a.removeFromTop (8 * 21);
    a.removeFromBottom (18);
    analysisMessage.setBounds (a);

    // ---- result card ----
    auto r = resultCard.reduced (14, 10);
    r.removeFromTop (22);
    auto titleRow = r.removeFromTop (22);
    resultTitle.setBounds (titleRow);
    resultMeta.setBounds (r.removeFromTop (18));
    r.removeFromTop (6);
    auto lower = r.removeFromBottom (30);
    r.removeFromBottom (8);
    waveform.setBounds (r);

    const int bw = (lower.getWidth() - 7 * 6) / 8;
    for (auto* b : { &playButton, &stopButton, &favoriteButton, &regenerateButton, &variationButton, &revealButton, &copyPathButton, &saveAsButton })
    {
        b->setBounds (lower.removeFromLeft (bw));
        lower.removeFromLeft (6);
    }
}
} // namespace context::ui
