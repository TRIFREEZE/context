#include "PromptPanel.h"
#include "Sheet.h"
#include "PluginProcessor.h"

namespace context::ui
{
PromptPanel::PromptPanel (ContextAudioProcessor& p) : processor (p)
{
    prompt.setMultiLine (true, true);
    prompt.setReturnKeyStartsNewLine (false);
    prompt.setScrollbarsShown (false);
    prompt.setFont (font (16.0f));
    prompt.setIndents (10, 9);
    prompt.setInputRestrictions (1000);
    prompt.setTextToShowWhenEmpty ("Describe the sound, e.g. dark distorted shredded electric guitar solo", colours::faint);
    prompt.onTextChange = [this] { pushUiState(); updatePromptPreview(); };
    prompt.onReturnKey = [this] { startGeneration(); };
    prompt.onEscapeKey = [this] { if (auto* top = getTopLevelComponent()) top->grabKeyboardFocus(); };
    addAndMakeVisible (prompt);

    juce::StringArray types, typeTips;
    for (auto t : allGenerationTypes)
    {
        types.add (toLabel (t));
        typeTips.add ("Generate a " + toLabel (t).toLowerCase());
    }
    typeSelector.setItems (types, typeTips);
    typeSelector.setProportional (true);
    typeSelector.onChange = [this] (int i)
    {
        auto s = processor.getUiState();
        s.type = allGenerationTypes[(size_t) i];
        processor.setUiState (s);
        updatePromptPreview();
    };
    addAndMakeVisible (typeSelector);

    juce::StringArray lengths, lengthTips;
    for (auto b : musical::lengthChoices)
    {
        lengths.add (juce::String (b));
        lengthTips.add (juce::String (b) + " bars: generated and imported sounds are fitted to exactly this length");
    }
    lengthSelector.setItems (lengths, lengthTips);
    lengthSelector.onChange = [this] (int i) { processor.session().setLengthBars (musical::lengthChoices[i]); };
    addAndMakeVisible (lengthSelector);

    setStyle (generateButton, ButtonStyle::Primary);
    generateButton.onClick = [this] { startGeneration(); };
    generateButton.addShortcut (juce::KeyPress (juce::KeyPress::returnKey, juce::ModifierKeys::ctrlModifier, 0));
    addAndMakeVisible (generateButton);
    cancelButton.onClick = [this] { processor.generation().cancelActive(); };
    addChildComponent (cancelButton);

    for (auto* b : { &useContext, &sendAudio, &expand })
    {
        setStyle (*b, ButtonStyle::Ghost);
        b->setClickingTogglesState (true);
        addAndMakeVisible (*b);
    }
    useContext.setTooltip ("Use the track analysis (tempo, key, mix balance) to shape the request. Nothing is uploaded.");
    useContext.onClick = [this] { pushUiState(); updatePromptPreview(); };
    sendAudio.onClick = [this]
    {
        if (sendAudio.getToggleState() && ! processor.services().settings().get().allowSendAudioContext)
        {
            sendAudio.setToggleState (false, juce::dontSendNotification);
            askToAllowAudio();
            return;
        }
        pushUiState();
        refresh();
    };
    contextMode.setProportional (true);
    contextMode.onChange = [this] (int i)
    {
        auto s = processor.getUiState();
        if (juce::isPositiveAndBelow (i, modeIds.size()))
            s.contextMode = modeIds[i];
        processor.setUiState (s);
    };
    addChildComponent (contextMode);
    expand.setTooltip ("Add context hints to your prompt. Off sends your words exactly as typed.");
    expand.onClick = [this]
    {
        const bool on = expand.getToggleState();
        processor.services().settings().update ([on] (Settings& s) { s.promptExpansion = on; });
        updatePromptPreview();
    };

    prompt.setText (processor.getUiState().prompt, juce::dontSendNotification);
    refresh();
}

void PromptPanel::askToAllowAudio()
{
    juce::Component::SafePointer<PromptPanel> safe (this);
    const bool shown = Sheet::show (*this, "Send your track to the generator?",
        "With this on, Context sends your captured 8 bars (or the last 8 seconds you played) to your generation bridge with "
        "each request, so the result can be made on your track: in time, in key and fitting the mix.\n\n"
        "Your bridge may upload it to its service. The Kie.ai bridge uploads it to Kie.ai's temporary file storage, which "
        "deletes it after 24 hours. Track audio is never sent to the Context licensing server. You can turn this off any "
        "time here or in Settings.",
        { { "Not now", ButtonStyle::Secondary, {} },
          { "Allow and turn on", ButtonStyle::Primary, [safe]
            {
                if (safe == nullptr) return;
                safe->processor.services().settings().update ([] (Settings& s) { s.allowSendAudioContext = true; });
                safe->sendAudio.setToggleState (true, juce::dontSendNotification);
                safe->pushUiState();
                safe->refresh();
            } } });
    if (! shown && onStatus)
        onStatus ("Allow sending your track in Settings first.");
}

void PromptPanel::focusPrompt()
{
    if (isShowing())
        prompt.grabKeyboardFocus();
}

void PromptPanel::pushUiState()
{
    auto s = processor.getUiState();
    s.prompt = prompt.getText();
    s.useTrackContext = useContext.getToggleState();
    s.sendAudioContext = sendAudio.getToggleState();
    processor.setUiState (s);
}

void PromptPanel::updatePromptPreview()
{
    const auto s = processor.getUiState();
    const auto out = processor.previewPrompt (s);
    juce::String text;
    if (s.prompt.trim().isEmpty())
        text = {};
    else if (! processor.services().settings().get().promptExpansion)
        text = "Sent as typed" + (out.bpm || out.key.isNotEmpty()
                   ? juce::String (" + ") + (out.bpm ? juce::String (juce::roundToInt (*out.bpm)) + " BPM " : juce::String()) + out.key + " " + out.mode
                   : juce::String());
    else
        text = out.expandedPrompt;
    if (text != preview)
    {
        preview = text;
        repaint (previewArea);
    }
}

void PromptPanel::startGeneration()
{
    pushUiState();
    if (processor.getUiState().prompt.trim().isEmpty())
    {
        if (onStatus) onStatus ("Type what you want to hear first.");
        prompt.grabKeyboardFocus();
        return;
    }
    const auto r = processor.generate();
    if (! r.ok && onError)
        onError (r.error);
    refresh();
}

void PromptPanel::refresh()
{
    const auto ui = processor.getUiState();
    const auto settings = processor.services().settings().get();
    const auto caps = processor.generation().getCapabilities();
    const auto job = processor.generation().getCurrentJob();
    const bool licensed = processor.isLicensed();
    busy = processor.generation().isBusy();

    if (! prompt.hasKeyboardFocus (true) && prompt.getText() != ui.prompt)
        prompt.setText (ui.prompt, juce::dontSendNotification);

    for (size_t i = 0; i < allGenerationTypes.size(); ++i)
    {
        const bool supported = caps.types.empty() || caps.supports (allGenerationTypes[i]);
        typeSelector.setItemEnabled ((int) i, supported);
        if (! supported)
            typeSelector.setItemTooltip ((int) i, "Not supported by the current generation provider");
        if (allGenerationTypes[i] == ui.type)
            typeSelector.setSelected ((int) i);
    }
    const int bars = processor.session().getState().lengthBars;
    for (int i = 0; i < (int) std::size (musical::lengthChoices); ++i)
        if (musical::lengthChoices[i] == bars)
            lengthSelector.setSelected (i);

    useContext.setToggleState (ui.useTrackContext, juce::dontSendNotification);
    const auto modes = caps.usableContextModes();
    const bool bridgeTakesAudio = ! modes.isEmpty();
    const bool sending = ui.sendAudioContext && settings.allowSendAudioContext && bridgeTakesAudio;
    sendAudio.setEnabled (bridgeTakesAudio);
    sendAudio.setToggleState (sending, juce::dontSendNotification);
    sendAudio.setTooltip (! bridgeTakesAudio ? "The current generation bridge can't use your track's audio (it still gets tempo and key)."
                          : ! settings.allowSendAudioContext ? "Send your captured bars with each request so the result is made on your track. Asks first."
                          : "Sends your captured bars with each request, so the result is made on your track.");
    if (modes != modeIds)
    {
        modeIds = modes;
        juce::StringArray labels, tips;
        for (auto& m : modes) { labels.add (contextmode::label (m)); tips.add (contextmode::tooltip (m)); }
        contextMode.setItems (labels, tips);
    }
    contextMode.setVisible (sending && modes.size() > 1);
    contextMode.setSelected (juce::jmax (0, modes.indexOf (ui.contextMode)));
    expand.setToggleState (settings.promptExpansion, juce::dontSendNotification);
    updatePromptPreview();

    generateButton.setEnabled (licensed);
    generateButton.setTooltip (licensed ? "Generate (Enter)" : "Activate Context in Settings to generate.");
    generateButton.setVisible (! busy);
    cancelButton.setVisible (busy);

    juce::String st;
    if (! licensed)                              st = "Activate Context to generate (Settings > License).";
    else if (busy)                               st = job.statusText.isNotEmpty() ? job.statusText : toString (job.state);
    else if (job.state == JobState::Cancelled)   st = "Cancelled.";
    else if (job.state == JobState::Failed)      st = "Generation failed.";
    if (busy && processor.generation().numQueued() > 0)
        st << "  (" << processor.generation().numQueued() << " queued)";
    if (st != status)
    {
        status = st;
        repaint (statusArea);
    }

    if (job.state == JobState::Failed && job.error.isError() && job.localJobId != lastFailedJobShown)
    {
        lastFailedJobShown = job.localJobId;
        if (onError) onError (job.error);
    }
    resized();
}

void PromptPanel::tick()
{
    const auto job = processor.generation().getCurrentJob();
    const bool nowBusy = processor.generation().isBusy();
    const double p = nowBusy ? job.progress : -2.0;
    if (nowBusy != busy)
        refresh();
    if (nowBusy && (std::abs (p - progress) > 1.0e-3 || p < 0.0))
    {
        progress = p;
        repaint (progressArea);
        const auto st = job.statusText.isNotEmpty() ? job.statusText : toString (job.state);
        if (st != status)
        {
            status = st;
            repaint (statusArea);
        }
    }
}

void PromptPanel::paint (juce::Graphics& g)
{
    paintHeading (g, "Type", typeLabelArea);
    paintHeading (g, "Bars", lengthLabelArea);
    if (contextMode.isVisible())
        paintHeading (g, "Use my track to", modeLabelArea);
    if (preview.isNotEmpty() && processor.services().settings().get().showEnhancedPrompt)
    {
        g.setColour (colours::faint);
        g.setFont (font (type::meta));
        g.drawText (fitText (preview, font (type::meta), (float) previewArea.getWidth()), previewArea, juce::Justification::centredLeft, false);
    }
    if (status.isNotEmpty())
    {
        g.setColour (busy ? colours::dim : colours::faint);
        g.setFont (font (type::label, Weight::Medium));
        g.drawText (fitText (status, font (type::label, Weight::Medium), (float) statusArea.getWidth()), statusArea, juce::Justification::centredRight, false);
    }
    if (busy)
        drawProgress (g, progressArea.toFloat(), progress);
}

void PromptPanel::resized()
{
    auto r = getLocalBounds().reduced (space::l, space::m);
    auto top = r.removeFromTop (54);
    auto action = top.removeFromRight (128);
    top.removeFromRight (space::s);
    prompt.setBounds (top);
    generateButton.setBounds (action);
    cancelButton.setBounds (action.withHeight (34));
    progressArea = action.withTrimmedTop (42).withHeight (3);

    r.removeFromTop (space::s);
    auto row = r.removeFromTop (height::control);
    typeLabelArea = row.removeFromLeft (36);
    const int typeW = juce::jmin (row.getWidth() - 360, juce::jmax (420, typeSelector.idealWidth()));
    typeSelector.setBounds (row.removeFromLeft (typeW));
    row.removeFromLeft (space::l);
    lengthLabelArea = row.removeFromLeft (36);
    lengthSelector.setBounds (row.removeFromLeft (150));
    row.removeFromLeft (space::l);
    for (auto* b : { &expand, &sendAudio, &useContext })
    {
        b->setBounds (row.removeFromRight (juce::GlyphArrangement::getStringWidthInt (font (type::body, Weight::Medium), b->getButtonText()) + 20));
        row.removeFromRight (2);
    }

    r.removeFromTop (space::xs + 2);
    auto info = r.removeFromTop (20);
    statusArea = info.removeFromRight (juce::jmin (320, info.getWidth() / 3));
    if (contextMode.isVisible())
    {
        modeLabelArea = info.removeFromLeft (juce::GlyphArrangement::getStringWidthInt (labelFont(), "Use my track to") + 10);
        contextMode.setBounds (info.removeFromLeft (juce::jmax (200, contextMode.idealWidth())).withSizeKeepingCentre (juce::jmax (200, contextMode.idealWidth()), 20));
        info.removeFromLeft (space::l);
    }
    previewArea = info.withTrimmedRight (space::s);
}
} // namespace context::ui
