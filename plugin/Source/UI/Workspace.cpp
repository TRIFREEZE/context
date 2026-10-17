#include "Workspace.h"
#include "PluginProcessor.h"
#include "Utils/Redaction.h"

namespace context::ui
{
Workspace::Workspace (ContextAudioProcessor& p)
    : processor (p), prompt (p), lanes (p), rack (p), transport (p)
{
    addAndMakeVisible (prompt);
    addAndMakeVisible (lanes);
    addAndMakeVisible (rack);
    addAndMakeVisible (transport);
    addChildComponent (toast);

    prompt.onError = [this] (const ProviderError& e) { showError (e); };
    prompt.onStatus = [this] (const juce::String& s) { toast.show (s, false); resized(); };
    transport.onError = [this] (const ProviderError& e) { showError (e); };

    processor.session().addChangeListener (this);
    lastNotice = processor.session().lastNotice().serial;
}

Workspace::~Workspace()
{
    processor.session().removeChangeListener (this);
}

void Workspace::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh();
}

void Workspace::refresh()
{
    prompt.refresh();
    lanes.refresh();
    rack.refresh();
    transport.refresh();
    const auto n = processor.session().lastNotice();
    if (n.serial != lastNotice)
    {
        lastNotice = n.serial;
        toast.show (n.text, n.error);
        resized();
    }
}

void Workspace::tick()
{
    prompt.tick();
    lanes.tick();
    rack.tick();
    transport.tick();
}

void Workspace::showError (const ProviderError& e)
{
    const auto msg = e.userMessage.isNotEmpty() ? e.userMessage : ProviderError::defaultUserMessage (e.code);
    toast.show (msg, true, [this, e] { showDetails (e); });
    resized();
}

void Workspace::showDetails (const ProviderError& e)
{
    juce::String detail;
    detail << "Error code: " << toId (e.code) << "\n";
    if (e.httpStatus != 0)
        detail << "HTTP status: " << e.httpStatus << "\n";
    detail << "\n" << redaction::redact (e.technicalDetail.isNotEmpty() ? e.technicalDetail : juce::String ("(no further detail)"))
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

void Workspace::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    g.setColour (colours::panel);
    g.fillRect (prompt.getBounds());
    paintSeparator (g, prompt.getBottom(), 0, getWidth());
}

void Workspace::resized()
{
    auto r = getLocalBounds();
    prompt.setBounds (r.removeFromTop (114));
    transport.setBounds (r.removeFromBottom (height::transport));
    // Processing gets a fixed share; the lanes take the rest (they grow with the window).
    const int rackH = juce::jlimit (196, 260, r.getHeight() * 40 / 100);
    rack.setBounds (r.removeFromBottom (rackH));
    lanes.setBounds (r.withTrimmedTop (space::s));

    if (toast.isVisible())
    {
        const int w = juce::jmin (toast.preferredWidth(), getWidth() - 40);
        toast.setBounds (juce::Rectangle<int> (w, 34).withCentre ({ getWidth() / 2, transport.getY() - 26 }));
    }
}
} // namespace context::ui
