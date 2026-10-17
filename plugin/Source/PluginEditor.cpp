#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "BuildInfo.h"

namespace context
{
ContextAudioProcessorEditor::ContextAudioProcessorEditor (ContextAudioProcessor& p)
    : juce::AudioProcessorEditor (p), processor (p), workspace (p), historyTab (p), settingsTab (p)
{
    setLookAndFeel (&lookAndFeel);

    for (auto* b : { &generateTabButton, &historyTabButton, &settingsTabButton })
    {
        ui::setStyle (*b, ui::ButtonStyle::Tab);
        addAndMakeVisible (*b);
    }
    generateTabButton.onClick = [this] { showTab ("generate"); };
    historyTabButton.onClick  = [this] { showTab ("history"); };
    settingsTabButton.onClick = [this] { showTab ("settings"); };

    addChildComponent (workspace);
    addChildComponent (historyTab);
    addChildComponent (settingsTab);
    historyTab.onUseResult = [this] { showTab ("generate"); };
    settingsTab.onRunSetupAgain = [this] { openOnboarding(); };

    auto& services = processor.services();
    processor.stateChanged.addChangeListener (this);
    processor.generation().addChangeListener (this);
    processor.analysis().addChangeListener (this);
    services.license().addChangeListener (this);
    services.settings().addChangeListener (this);
    services.history().addChangeListener (this);

    if (! services.settings().get().firstRunComplete)
        openOnboarding();

    // Connection status is tested when the window first opens (never during plugin scans).
    if (processor.generation().getConnectionResult().status == ConnectionStatus::Unknown)
        processor.generation().testConnection();
    services.license().refreshIfDue();
    const auto s = services.settings().get();
    if (s.checkForUpdates && juce::Time::currentTimeMillis() - s.lastUpdateCheckMs > 24LL * 3600 * 1000)
    {
        services.settings().update ([] (Settings& st) { st.lastUpdateCheckMs = juce::Time::currentTimeMillis(); });
        services.license().checkForUpdate();
    }

    setResizable (true, true);
    setResizeLimits (980, 700, 2200, 1500);
    setSize (1120, 800);

    showTab (processor.getUiState().tab);
    startTimerHz (30);
}

ContextAudioProcessorEditor::~ContextAudioProcessorEditor()
{
    stopTimer();
    auto& services = processor.services();
    processor.stateChanged.removeChangeListener (this);
    processor.generation().removeChangeListener (this);
    processor.analysis().removeChangeListener (this);
    services.license().removeChangeListener (this);
    services.settings().removeChangeListener (this);
    services.history().removeChangeListener (this);
    onboarding.reset();
    setLookAndFeel (nullptr);
}

void ContextAudioProcessorEditor::openOnboarding()
{
    onboarding = std::make_unique<ui::Onboarding> (processor);
    onboarding->onFinished = [this]
    {
        // Destroy the overlay after its button callback has returned.
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ContextAudioProcessorEditor> (this)]
        {
            if (safe != nullptr)
            {
                safe->onboarding.reset();
                safe->refreshAll();
            }
        });
    };
    addAndMakeVisible (*onboarding);
    onboarding->setBounds (getLocalBounds());
}

void ContextAudioProcessorEditor::showTab (const juce::String& id)
{
    currentTab = (id == "history" || id == "settings") ? id : "generate";
    workspace.setVisible (currentTab == "generate");
    historyTab.setVisible (currentTab == "history");
    settingsTab.setVisible (currentTab == "settings");
    generateTabButton.setToggleState (currentTab == "generate", juce::dontSendNotification);
    historyTabButton.setToggleState (currentTab == "history", juce::dontSendNotification);
    settingsTabButton.setToggleState (currentTab == "settings", juce::dontSendNotification);

    auto s = processor.getUiState();
    if (s.tab != currentTab)
    {
        s.tab = currentTab;
        processor.setUiState (s);
    }
    refreshAll();
    if (currentTab == "generate")
        workspace.focusPrompt();
}

void ContextAudioProcessorEditor::refreshAll()
{
    if (currentTab == "generate") workspace.refresh();
    if (currentTab == "history")  historyTab.refresh();
    if (currentTab == "settings") settingsTab.refresh();
    if (onboarding != nullptr)    onboarding->refresh();
    repaint (0, 0, getWidth(), ui::height::topBar);
}

void ContextAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshAll();
}

void ContextAudioProcessorEditor::timerCallback()
{
    if (currentTab == "generate")
        workspace.tick();
    if (++ticks % 15 == 0)
        repaint (0, 0, getWidth(), ui::height::topBar);   // connection / license status
}

void ContextAudioProcessorEditor::mouseUp (const juce::MouseEvent& e)
{
    if (statusPill.contains (e.getPosition()) || licenseChip.contains (e.getPosition()))
        showTab ("settings");
}

void ContextAudioProcessorEditor::paint (juce::Graphics& g)
{
    using namespace ui;
    g.fillAll (colours::background);

    auto header = getLocalBounds().removeFromTop (height::topBar);
    g.setColour (colours::panel);
    g.fillRect (header);
    paintSeparator (g, header.getBottom() - 1, 0, getWidth());

    // Wordmark
    g.setColour (colours::text);
    g.setFont (ui::font (13.0f, Weight::SemiBold).withExtraKerningFactor (0.32f));
    g.drawText (juce::String (build::productName).toUpperCase(), header.withTrimmedLeft (space::l).withWidth (120), juce::Justification::centredLeft);

    // Active tab underline
    for (auto* b : { &generateTabButton, &historyTabButton, &settingsTabButton })
        if (b->getToggleState())
        {
            g.setColour (colours::accent);
            g.fillRect (b->getX() + 10, header.getBottom() - 2, b->getWidth() - 20, 2);
        }

    // Provider status: dot + text, quiet unless something is wrong.
    const auto conn = processor.generation().getConnectionResult();
    const bool mock = processor.services().settings().get().providerId == "mock";
    juce::Colour dot = colours::faint;
    juce::String text;
    switch (conn.status)
    {
        case ConnectionStatus::Connected:  dot = colours::good; text = conn.bridgeName.isNotEmpty() ? conn.bridgeName : "Bridge connected"; break;
        case ConnectionStatus::MockReady:  dot = colours::warn; text = "Mock provider (test sounds)"; break;
        case ConnectionStatus::Testing:    dot = colours::faint; text = mock ? "Mock provider" : "Connecting..."; break;
        case ConnectionStatus::Unknown:    dot = colours::faint; text = mock ? "Mock provider" : "Bridge not tested"; break;
        case ConnectionStatus::ConnectionRefused:
        case ConnectionStatus::Disconnected: dot = colours::bad; text = "Bridge offline"; break;
        default:                           dot = colours::bad;  text = "Bridge: " + toDisplayString (conn.status); break;
    }
    const auto f = ui::font (11.5f, Weight::Medium);
    const int textW = juce::jmin (260, juce::GlyphArrangement::getStringWidthInt (f, text));
    statusPill = juce::Rectangle<int> (getWidth() - space::l - textW - 18, header.getY(), textW + 18, header.getHeight());
    g.setColour (dot);
    g.fillEllipse ((float) statusPill.getX(), (float) statusPill.getCentreY() - 3.5f, 7.0f, 7.0f);
    g.setColour (conn.ok() || conn.status == ConnectionStatus::Testing ? colours::dim : colours::text);
    g.setFont (f);
    g.drawText (text, statusPill.withTrimmedLeft (14), juce::Justification::centredLeft, true);

    // License: only shown when it needs attention.
    const bool licensed = processor.isLicensed();
    if (! licensed)
    {
        const auto chipText = LicenseManager::toString (processor.services().license().getStatus());
        const int chipW = juce::GlyphArrangement::getStringWidthInt (f, chipText) + 20;
        licenseChip = juce::Rectangle<int> (statusPill.getX() - chipW - space::m, header.getCentreY() - 11, chipW, 22);
        g.setColour (colours::warn.withAlpha (0.12f));
        g.fillRoundedRectangle (licenseChip.toFloat(), radius::control);
        g.setColour (colours::warn);
        g.drawText (chipText, licenseChip, juce::Justification::centred);
    }
    else
    {
        licenseChip = {};
    }
}

void ContextAudioProcessorEditor::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (ui::height::topBar);
    auto tabs = header.withTrimmedLeft (132).withWidth (300);
    for (auto* b : { &generateTabButton, &historyTabButton, &settingsTabButton })
    {
        b->setBounds (tabs.removeFromLeft (84).withSizeKeepingCentre (84, 28));
        tabs.removeFromLeft (2);
    }
    workspace.setBounds (r);
    historyTab.setBounds (r);
    settingsTab.setBounds (r);
    if (onboarding != nullptr)
        onboarding->setBounds (getLocalBounds());
}
} // namespace context
