#pragma once

#include "PromptPanel.h"
#include "LanesPanel.h"
#include "EffectRack.h"
#include "TransportBar.h"

namespace context { class ContextAudioProcessor; }

namespace context::ui
{
/** The main view: prompt -> track context + sound lanes -> processing -> transport.
    Only composes panels and routes refresh/tick; the panels own their behaviour and
    talk to the processor/session, never to each other. */
class Workspace : public juce::Component, private juce::ChangeListener
{
public:
    explicit Workspace (ContextAudioProcessor&);
    ~Workspace() override;

    void refresh();
    void tick();
    void focusPrompt() { prompt.focusPrompt(); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void showError (const ProviderError&);
    void showDetails (const ProviderError&);

    ContextAudioProcessor& processor;
    PromptPanel prompt;
    LanesPanel lanes;
    EffectRackPanel rack;
    TransportBar transport;
    Toast toast;
    int lastNotice = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Workspace)
};
} // namespace context::ui
