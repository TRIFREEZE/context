#include "TransportBar.h"
#include "PluginProcessor.h"

namespace context::ui
{
// ===========================================================================
// Drag pill
// ===========================================================================
void DragToDawPill::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    const bool over = isMouseOver();
    const bool enabled = isEnabled();
    g.setColour (enabled ? (over ? colours::accentDark : colours::accentDark.withAlpha (0.55f)) : colours::raised);
    g.fillRoundedRectangle (r, radius::control);
    g.setColour (enabled ? colours::accent.withAlpha (over ? 1.0f : 0.7f) : colours::border);
    g.drawRoundedRectangle (r.reduced (0.5f), radius::control, 1.0f);
    auto content = getLocalBounds().reduced (10, 0);
    IconButton::drawIcon (g, IconButton::Icon::Drag, content.removeFromLeft (12).toFloat().withSizeKeepingCentre (10.0f, 14.0f),
                          enabled ? colours::accent : colours::disabled);
    content.removeFromLeft (8);
    g.setColour (enabled ? colours::text : colours::disabled.brighter (0.2f));
    g.setFont (font (12.0f, Weight::SemiBold));
    g.drawText ("Drag to DAW", content, juce::Justification::centredLeft);
}

void DragToDawPill::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || ! isEnabled() || e.getDistanceFromDragStart() < 5 || ! onDragOut)
        return;
    dragging = true;
    const auto f = onDragOut();
    if (f.existsAsFile())
        juce::DragAndDropContainer::performExternalDragDropOfFiles ({ f.getFullPathName() }, false, this);
}

void DragToDawPill::mouseUp (const juce::MouseEvent&)
{
    if (! dragging && onClickedWithoutDrag)
        onClickedWithoutDrag();
    dragging = false;
}

// ===========================================================================
// Transport
// ===========================================================================
TransportBar::TransportBar (ContextAudioProcessor& p) : processor (p)
{
    auto& session = processor.session();
    setStyle (play, ButtonStyle::Primary);
    play.onClick = [this] { processor.preview().stop(); processor.session().togglePlay(); refresh(); };
    rewind.onClick = [this] { processor.session().rewind(); };
    loop.setClickingTogglesState (true);
    loop.onClick = [this] { processor.session().setLoop (loop.getToggleState()); };
    for (auto* b : { &rewind, &play, &loop })
        addAndMakeVisible (*b);

    mode.setItems ({ "Both", "Context", "Output" }, { "Hear the track context and the sound together (default)", "Hear only your captured track", "Hear only the generated / imported sound" });
    mode.onChange = [this] (int i)
    {
        const PreviewEngine::Mode m[] = { PreviewEngine::Mode::Both, PreviewEngine::Mode::Context, PreviewEngine::Mode::Output };
        processor.session().setPlayMode (m[i]);   // switches instantly while playing
        refresh();
    };
    addAndMakeVisible (mode);

    favorite.setOnColour (colours::favorite);
    favorite.onClick = [this]
    {
        if (auto r = processor.getSelectedResult())
            processor.services().history().setFavorite (r->generationId, ! r->favorite);
    };
    for (auto* b : { &regenerate, &variation })
    {
        setStyle (*b, ButtonStyle::Ghost);
        addAndMakeVisible (*b);
    }
    regenerate.setTooltip ("Generate again with the same prompt and context");
    regenerate.onClick = [this]
    {
        if (auto r = processor.getSelectedResult())
        {
            auto res = processor.regenerate (*r);
            if (! res.ok && onError) onError (res.error);
        }
    };
    variation.onClick = [this]
    {
        if (auto r = processor.getSelectedResult())
        {
            auto res = processor.variation (*r);
            if (! res.ok && onError) onError (res.error);
        }
    };
    saveButton.onClick = [this] { saveAs(); };
    revealButton.onClick = [this] { const auto f = exportOrNotify(); if (f.existsAsFile()) f.revealToUser(); };
    copyButton.onClick = [this]
    {
        const auto f = exportOrNotify();
        if (f.existsAsFile())
        {
            juce::SystemClipboard::copyTextToClipboard (f.getFullPathName());
            processor.session().notify ("Path copied. Paste it into your DAW's browser.");
        }
    };
    for (auto* b : { &favorite, &saveButton, &revealButton, &copyButton })
        addAndMakeVisible (*b);

    drag.setTooltip ("Drag the processed sound (fit + effects, exactly as you hear it) into your DAW");
    drag.onDragOut = [this] { return exportOrNotify(); };
    drag.onClickedWithoutDrag = [this] { processor.session().notify ("Press and drag this onto your DAW's playlist or a channel."); };
    addAndMakeVisible (drag);
    juce::ignoreUnused (session);
    refresh();
}

juce::File TransportBar::exportOrNotify()
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
}

void TransportBar::saveAs()
{
    const auto src = exportOrNotify();
    if (! src.existsAsFile())
        return;
    chooser = std::make_unique<juce::FileChooser> ("Save the processed sound",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (src.getFileName()), "*.wav");
    juce::Component::SafePointer<TransportBar> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, src] (const juce::FileChooser& fc)
    {
        const auto dest = fc.getResult();
        if (dest == juce::File() || safe == nullptr)
            return;
        const auto target = dest.hasFileExtension ("wav") ? dest : dest.withFileExtension ("wav");
        const bool ok = target != src && src.copyFileTo (target);
        safe->processor.session().notify (ok ? "Saved " + target.getFileName() : "Couldn't save the copy.", ! ok);
    });
}

void TransportBar::refresh()
{
    auto& s = processor.session();
    const auto st = s.getState();
    const bool playing = s.isPlaying();
    play.setIcon (playing ? IconButton::Icon::Stop : IconButton::Icon::Play);
    play.setTooltip (playing ? "Stop" : "Play both lanes in sync");
    loop.setToggleState (st.loop, juce::dontSendNotification);
    mode.setSelected (st.playMode == PreviewEngine::Mode::Both ? 0 : st.playMode == PreviewEngine::Mode::Context ? 1 : 2);

    const bool generated = st.sourceKind == SessionState::SourceKind::Generated;
    const auto result = generated ? processor.getSelectedResult() : std::nullopt;
    const bool busy = processor.generation().isBusy();
    favorite.setEnabled (result.has_value());
    favorite.setIcon (result && result->favorite ? IconButton::Icon::StarFilled : IconButton::Icon::Star);
    favorite.setToggleState (result && result->favorite, juce::dontSendNotification);
    regenerate.setEnabled (result.has_value() && processor.isLicensed() && ! busy);
    const bool canVar = processor.canVariation();
    variation.setEnabled (result.has_value() && processor.isLicensed() && ! busy && canVar);
    variation.setTooltip (canVar ? "Create a variation of this sound" : "The current provider doesn't support variations");
    const bool hasOutput = s.outputLane().hasAudio;
    for (juce::Component* c : { (juce::Component*) &saveButton, (juce::Component*) &revealButton, (juce::Component*) &copyButton, (juce::Component*) &drag })
        c->setEnabled (hasOutput);
    drag.repaint();
}

void TransportBar::tick()
{
    auto& s = processor.session();
    const bool playing = s.isPlaying();
    if ((play.getProperties()["isStop"] == juce::var (true)) != playing)
    {
        play.getProperties().set ("isStop", playing);
        refresh();
    }
    // Position: bar.beat on the shared timeline
    const auto tempo = s.sessionTempo();
    const double spb = musical::samplesPerBar (tempo, s.renderRate());
    juce::String text;
    if (spb > 0.0)
    {
        const auto pos = s.playheadSamples();
        const double bars = (double) pos / spb;
        const int bar = (int) bars + 1;
        const int beat = (int) ((bars - std::floor (bars)) * juce::jmax (1, tempo.signature.numerator)) + 1;
        double total = (double) s.timelineSamples() / spb;
        if (total <= 0.0)
            total = juce::jmax (s.getState().lengthBars, s.hasContext() ? musical::contextBars : 0);
        text << bar << "." << beat << "  /  " << juce::roundToInt (total) << " bars";
    }
    if (text != positionText)
    {
        positionText = text;
        repaint (positionArea);
    }
}

void TransportBar::paint (juce::Graphics& g)
{
    g.setColour (colours::panel);
    g.fillRect (getLocalBounds());
    paintSeparator (g, 0, 0, getWidth());
    g.setColour (colours::dim);
    g.setFont (numberFont (12.0f));
    g.drawText (positionText, positionArea, juce::Justification::centredLeft);
}

void TransportBar::resized()
{
    auto r = getLocalBounds().reduced (space::l, 0).withSizeKeepingCentre (getWidth() - 2 * space::l, 28);
    rewind.setBounds (r.removeFromLeft (28));
    r.removeFromLeft (4);
    play.setBounds (r.removeFromLeft (40));
    r.removeFromLeft (4);
    loop.setBounds (r.removeFromLeft (28));
    r.removeFromLeft (space::m);
    mode.setBounds (r.removeFromLeft (190).withSizeKeepingCentre (190, height::control));
    r.removeFromLeft (space::l);
    positionArea = r.removeFromLeft (120);

    drag.setBounds (r.removeFromRight (124));
    r.removeFromRight (space::s);
    for (auto* b : { &copyButton, &revealButton, &saveButton })
    {
        b->setBounds (r.removeFromRight (28));
        r.removeFromRight (2);
    }
    r.removeFromRight (space::m);
    variation.setBounds (r.removeFromRight (78).withSizeKeepingCentre (78, height::control));
    regenerate.setBounds (r.removeFromRight (88).withSizeKeepingCentre (88, height::control));
    favorite.setBounds (r.removeFromRight (28));
}

// ===========================================================================
// Toast
// ===========================================================================
Toast::Toast()
{
    setStyle (detailsButton, ButtonStyle::Ghost);
    detailsButton.onClick = [this] { if (onDetails) onDetails(); };
    addChildComponent (detailsButton);
    close.onClick = [this] { setVisible (false); stopTimer(); };
    addAndMakeVisible (close);
    setVisible (false);
}

int Toast::preferredWidth() const
{
    return juce::jlimit (240, 640, juce::GlyphArrangement::getStringWidthInt (font (12.5f, Weight::Medium), message) + 32 + 24 + (onDetails ? 70 : 0));
}

void Toast::show (const juce::String& text, bool error, std::function<void()> details)
{
    message = text;
    isError = error;
    onDetails = std::move (details);
    detailsButton.setVisible (onDetails != nullptr);
    setVisible (true);
    toFront (false);
    if (auto* p = getParentComponent()) p->resized();
    resized();
    repaint();
    startTimer (error ? 9000 : 4000);
}

void Toast::timerCallback()
{
    stopTimer();
    setVisible (false);
}

void Toast::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (colours::raised);
    g.fillRoundedRectangle (r, radius::panel);
    g.setColour (isError ? colours::bad.withAlpha (0.7f) : colours::border);
    g.drawRoundedRectangle (r, radius::panel, 1.0f);
    auto t = getLocalBounds().reduced (12, 0);
    g.setColour (isError ? colours::bad : colours::accent);
    g.fillEllipse ((float) t.getX(), (float) getHeight() * 0.5f - 3.0f, 6.0f, 6.0f);
    t.removeFromLeft (14);
    t.removeFromRight (24 + (onDetails ? 70 : 0));
    g.setColour (colours::text);
    g.setFont (font (12.5f, Weight::Medium));
    g.drawText (fitText (message, font (12.5f, Weight::Medium), (float) t.getWidth()), t, juce::Justification::centredLeft, false);
}

void Toast::resized()
{
    auto r = getLocalBounds().reduced (6, 0);
    close.setBounds (r.removeFromRight (22).withSizeKeepingCentre (22, 22));
    if (detailsButton.isVisible())
        detailsButton.setBounds (r.removeFromRight (66).withSizeKeepingCentre (66, 22));
}

void Toast::mouseUp (const juce::MouseEvent&)
{
    setVisible (false);
}
} // namespace context::ui
