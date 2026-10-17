#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "BuildInfo.h"

namespace context
{
ContextAudioProcessorEditor::ContextAudioProcessorEditor (ContextAudioProcessor& p)
    : juce::AudioProcessorEditor (p), processor (p), generateTab (p), historyTab (p), settingsTab (p)
{
    setLookAndFeel (&lookAndFeel);

    for (auto* b : { &generateTabButton, &historyTabButton, &settingsTabButton })
    {
        ui::styleQuiet (*b);
        b->setColour (juce::TextButton::buttonColourId, ui::colours::background);
        addAndMakeVisible (*b);
    }
    generateTabButton.onClick = [this] { showTab ("generate"); };
    historyTabButton.onClick  = [this] { showTab ("history"); };
    settingsTabButton.onClick = [this] { showTab ("settings"); };

    addChildComponent (generateTab);
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
    setResizeLimits (800, 640, 1600, 1100);
    setSize (920, 720);

    showTab (processor.getUiState().tab);
    startTimerHz (20);
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
    generateTab.setVisible (currentTab == "generate");
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
        generateTab.focusPrompt();
}

void ContextAudioProcessorEditor::refreshAll()
{
    if (currentTab == "generate") generateTab.refresh();
    if (currentTab == "history")  historyTab.refresh();
    if (currentTab == "settings") settingsTab.refresh();
    if (onboarding != nullptr)    onboarding->refresh();
    repaint (0, 0, getWidth(), 56);
}

void ContextAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshAll();
}

void ContextAudioProcessorEditor::timerCallback()
{
    if (currentTab == "generate")
        generateTab.tick();
    if (++ticks % 10 == 0)
        repaint (0, 0, getWidth(), 56);   // status pill
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

    auto header = getLocalBounds().removeFromTop (56);
    g.setColour (colours::panel);
    g.fillRect (header);
    g.setColour (colours::border);
    g.drawHorizontalLine (header.getBottom() - 1, 0.0f, (float) getWidth());

    // Wordmark
    g.setColour (colours::text);
    g.setFont (ContextLookAndFeel::font (20.0f, true).withExtraKerningFactor (0.22f));
    g.drawText (juce::String (build::productName).toUpperCase(), header.withTrimmedLeft (20).withWidth (170), juce::Justification::centredLeft);

    // Provider status pill
    const auto conn = processor.generation().getConnectionResult();
    const bool mock = processor.services().settings().get().providerId == "mock";
    juce::Colour dot = colours::dim;
    juce::String text;
    switch (conn.status)
    {
        case ConnectionStatus::Connected:  dot = colours::good; text = "Bridge: Connected"; break;
        case ConnectionStatus::MockReady:  dot = colours::warn; text = "Mock Provider (test)"; break;
        case ConnectionStatus::Testing:    dot = colours::dim;  text = mock ? "Mock Provider" : "Bridge: Testing..."; break;
        case ConnectionStatus::Unknown:    dot = colours::dim;  text = mock ? "Mock Provider" : "Bridge: Not tested"; break;
        case ConnectionStatus::ConnectionRefused:
        case ConnectionStatus::Disconnected: dot = colours::bad; text = "Bridge: Offline"; break;
        default:                           dot = colours::bad;  text = "Bridge: " + toDisplayString (conn.status); break;
    }
    g.setFont (ContextLookAndFeel::font (13.0f, true));
    const int pillW = juce::jmin (280, juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 38);
    statusPill = juce::Rectangle<int> (getWidth() - pillW - 16, header.getCentreY() - 14, pillW, 28);
    g.setColour (colours::raised);
    g.fillRoundedRectangle (statusPill.toFloat(), 14.0f);
    g.setColour (dot);
    g.fillEllipse ((float) statusPill.getX() + 12.0f, (float) statusPill.getCentreY() - 4.0f, 8.0f, 8.0f);
    g.setColour (colours::text);
    g.drawText (text, statusPill.withTrimmedLeft (28).withTrimmedRight (10), juce::Justification::centredLeft, true);

    // License chip
    const bool licensed = processor.isLicensed();
    const auto chipText = licensed ? juce::String ("Licensed") : LicenseManager::toString (processor.services().license().getStatus());
    const int chipW = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), chipText) + 24;
    licenseChip = juce::Rectangle<int> (statusPill.getX() - chipW - 8, statusPill.getY(), chipW, 28);
    g.setColour (licensed ? colours::raised : colours::warn.withAlpha (0.15f));
    g.fillRoundedRectangle (licenseChip.toFloat(), 14.0f);
    g.setColour (licensed ? colours::dim : colours::warn);
    g.drawText (chipText, licenseChip, juce::Justification::centred);
}

void ContextAudioProcessorEditor::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (56);
    auto tabs = header.withTrimmedLeft (210).withWidth (330).withSizeKeepingCentre (330, 32);
    for (auto* b : { &generateTabButton, &historyTabButton, &settingsTabButton })
    {
        b->setBounds (tabs.removeFromLeft (104));
        tabs.removeFromLeft (6);
    }
    generateTab.setBounds (r);
    historyTab.setBounds (r);
    settingsTab.setBounds (r);
    if (onboarding != nullptr)
        onboarding->setBounds (getLocalBounds());
}
} // namespace context
