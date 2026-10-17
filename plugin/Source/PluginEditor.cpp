#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "BuildInfo.h"

namespace context
{
ContextAudioProcessorEditor::ContextAudioProcessorEditor (ContextAudioProcessor& p)
    : juce::AudioProcessorEditor (p), processor (p), workspace (p), historyTab (p), settingsTab (p)
{
    setLookAndFeel (&lookAndFeel);
    setWantsKeyboardFocus (true);
    ui::motion::setEnabled (! processor.services().settings().get().reduceMotion);
    frameClock = ui::motion::Clock::instance().attach (*this);

    for (auto* b : { &generateTabButton, &historyTabButton, &settingsTabButton })
    {
        ui::setStyle (*b, ui::ButtonStyle::Tab);
        addAndMakeVisible (*b);
    }
    generateTabButton.onClick = [this] { showTab ("generate"); };
    historyTabButton.onClick  = [this] { showTab ("history"); };
    settingsTabButton.onClick = [this] { showTab ("settings"); };

    undoButton.onClick = [this] { processor.session().undo(); refreshUndoButtons(); };
    redoButton.onClick = [this] { processor.session().redo(); refreshUndoButtons(); };
    keysButton.onClick = [this] { showShortcuts(); };
    for (auto* b : { &undoButton, &redoButton, &keysButton })
        addAndMakeVisible (*b);

    addChildComponent (workspace);
    addChildComponent (historyTab);
    addChildComponent (settingsTab);
    historyTab.onUseResult = [this] { showTab ("generate"); };
    settingsTab.onRunSetupAgain = [this] { openOnboarding(); };
    workspace.onShowShortcuts = [this] { showShortcuts(); };

    tabUnderline.onChange ([this] { repaint (0, ui::height::topBar - 4, getWidth(), 4); });
    tabFade.onChange = [this]
    {
        if (auto* c = tabComponent (currentTab))
            c->setAlpha (juce::jlimit (0.0f, 1.0f, tabFade.get()));
    };

    auto& services = processor.services();
    processor.stateChanged.addChangeListener (this);
    processor.generation().addChangeListener (this);
    processor.analysis().addChangeListener (this);
    processor.session().addChangeListener (this);
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
    frameClock.reset();
    auto& services = processor.services();
    processor.stateChanged.removeChangeListener (this);
    processor.generation().removeChangeListener (this);
    processor.analysis().removeChangeListener (this);
    processor.session().removeChangeListener (this);
    services.license().removeChangeListener (this);
    services.settings().removeChangeListener (this);
    services.history().removeChangeListener (this);
    sheet.reset();
    onboarding.reset();
    setLookAndFeel (nullptr);
}

juce::Component* ContextAudioProcessorEditor::tabComponent (const juce::String& id)
{
    if (id == "history")  return &historyTab;
    if (id == "settings") return &settingsTab;
    return &workspace;
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

void ContextAudioProcessorEditor::presentSheet (std::unique_ptr<ui::Sheet> s)
{
    if (sheet != nullptr)
        removeChildComponent (sheet.get());
    sheet = std::move (s);
    addAndMakeVisible (*sheet);
    sheet->setBounds (getLocalBounds());
    sheet->appear();
}

void ContextAudioProcessorEditor::dismissSheet (ui::Sheet* s)
{
    if (s != nullptr && s == sheet.get())
    {
        removeChildComponent (sheet.get());
        sheet.reset();
        grabKeyboardFocus();
    }
}

void ContextAudioProcessorEditor::showTab (const juce::String& id)
{
    const auto previous = currentTab;
    currentTab = (id == "history" || id == "settings") ? id : "generate";
    workspace.setVisible (currentTab == "generate");
    historyTab.setVisible (currentTab == "history");
    settingsTab.setVisible (currentTab == "settings");
    generateTabButton.setToggleState (currentTab == "generate", juce::dontSendNotification);
    historyTabButton.setToggleState (currentTab == "history", juce::dontSendNotification);
    settingsTabButton.setToggleState (currentTab == "settings", juce::dontSendNotification);

    // The underline slides to the new tab and the page fades in (only when it changed).
    auto* active = currentTab == "history" ? &historyTabButton : currentTab == "settings" ? &settingsTabButton : &generateTabButton;
    const auto target = juce::Rectangle<float> ((float) active->getX() + 10.0f, (float) ui::height::topBar - 2.0f, (float) active->getWidth() - 20.0f, 2.0f);
    if (active->getWidth() > 0)
        tabUnderline.set (target, ui::motion::duration::slide, ui::motion::Ease::OutQuint);
    for (auto* c : { (juce::Component*) &workspace, (juce::Component*) &historyTab, (juce::Component*) &settingsTab })
        c->setAlpha (1.0f);
    if (previous != currentTab && isShowing())
    {
        tabFade.snap (0.0f);
        tabFade.set (1.0f, ui::motion::duration::fade, ui::motion::Ease::OutCubic);
    }

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
    refreshUndoButtons();
    ui::motion::setEnabled (! processor.services().settings().get().reduceMotion);
    repaint (0, 0, getWidth(), ui::height::topBar);
}

void ContextAudioProcessorEditor::refreshUndoButtons()
{
    auto& s = processor.session();
    undoButton.setEnabled (s.canUndo());
    redoButton.setEnabled (s.canRedo());
}

void ContextAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshAll();
}

void ContextAudioProcessorEditor::timerCallback()
{
    if (currentTab == "generate")
        workspace.tick();
    // A bridge that was offline (or started after the DAW) is picked up without a manual test:
    // re-test every 10 s while the window is open and the last test didn't connect.
    if (ticks % 300 == 299)
    {
        const auto st = processor.generation().getConnectionResult().status;
        if (st == ConnectionStatus::Unknown || st == ConnectionStatus::Disconnected || st == ConnectionStatus::ConnectionRefused)
            processor.generation().testConnection();
    }
    if (++ticks % 15 == 0)
    {
        refreshUndoButtons();
        repaint (0, 0, getWidth(), ui::height::topBar);   // connection / license status
    }
}

void ContextAudioProcessorEditor::mouseUp (const juce::MouseEvent& e)
{
    if (statusPill.contains (e.getPosition()) || licenseChip.contains (e.getPosition()))
        showTab ("settings");
    else
        grabKeyboardFocus();   // clicking empty space returns keys to the shortcuts
}

bool ContextAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    if (sheet != nullptr || onboarding != nullptr)
        return false;
    const auto mods = key.getModifiers();
    const bool cmd = mods.isCommandDown();
    const auto code = (int) juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) key.getKeyCode());
    if (cmd && code == 'Z' && ! mods.isShiftDown())  { processor.session().undo(); refreshUndoButtons(); return true; }
    if (cmd && ((code == 'Z' && mods.isShiftDown()) || code == 'Y')) { processor.session().redo(); refreshUndoButtons(); return true; }
    if (cmd && code == '1') { showTab ("generate"); return true; }
    if (cmd && code == '2') { showTab ("history"); return true; }
    if (cmd && code == '3') { showTab ("settings"); return true; }
    if (key.getTextCharacter() == '?' || key == juce::KeyPress::F1Key) { showShortcuts(); return true; }
    if (currentTab == "generate")
        return workspace.handleShortcut (key);
    return false;
}

void ContextAudioProcessorEditor::showShortcuts()
{
    // Two columns: action, keys. Built as a small component so it reads like a reference card.
    struct Card : juce::Component
    {
        juce::StringArray rows;
        void paint (juce::Graphics& g) override
        {
            using namespace ui;
            const int rowH = 22;
            const int colW = getWidth() / 2;
            for (int i = 0; i < rows.size(); ++i)
            {
                const auto parts = juce::StringArray::fromTokens (rows[i], "|", "");
                const int col = i < (rows.size() + 1) / 2 ? 0 : 1;
                const int row = col == 0 ? i : i - (rows.size() + 1) / 2;
                auto r = juce::Rectangle<int> (col * colW, row * rowH, colW - 16, rowH);
                g.setColour (colours::dim);
                g.setFont (font (type::body));
                g.drawText (parts[0], r, juce::Justification::centredLeft, true);
                const auto keys = parts[1];
                const auto kf = numberFont (type::meta, Weight::SemiBold);
                const int kw = juce::GlyphArrangement::getStringWidthInt (kf, keys) + 12;
                auto chip = r.removeFromRight (kw).withSizeKeepingCentre (kw, 18).toFloat();
                g.setColour (colours::raised);
                g.fillRoundedRectangle (chip, radius::small);
                g.setColour (colours::border);
                g.drawRoundedRectangle (chip.reduced (0.5f), radius::small, 1.0f);
                g.setColour (colours::text);
                g.setFont (kf);
                g.drawText (keys, chip, juce::Justification::centred);
            }
        }
    };
    auto card = std::make_unique<Card>();
    card->rows = { "Play / stop|Space", "Back to bar 1|Home", "Loop on / off|L", "Hear both / track / sound|1  2  3",
                   "Original / processed|A", "Capture 8 bars|R", "Generate|Ctrl+Enter", "Undo|Ctrl+Z", "Redo|Ctrl+Shift+Z",
                   "Select all of the sound|Ctrl+A", "Split at playhead or selection|S", "Delete selection|Del",
                   "Fill selection with a new take|F", "Copy / cut / paste|Ctrl+C X V", "Duplicate selection|Ctrl+D",
                   "Snap to grid on / off|G", "Clear selection|Esc", "Pages|Ctrl+1 2 3" };
    const int rows = (card->rows.size() + 1) / 2;
    ui::Sheet::show (*this, "Keyboard shortcuts",
                     "Shortcuts work when the plugin window has focus (click an empty spot first). In FL Studio, "
                     "keys typed into the prompt stay in the prompt.",
                     { { "Done", ui::ButtonStyle::Primary, {} } }, std::move (card), rows * 22);
}

void ContextAudioProcessorEditor::paint (juce::Graphics& g)
{
    using namespace ui;
    g.fillAll (colours::background);

    auto header = getLocalBounds().removeFromTop (height::topBar);
    g.setColour (colours::panel);
    g.fillRect (header);
    paintSeparator (g, header.getBottom() - 1, 0, getWidth());

    // Wordmark: the product name set like a hardware nameplate.
    g.setColour (colours::text);
    g.setFont (ui::font (15.0f, Weight::SemiBold).withExtraKerningFactor (0.18f));
    g.drawText (juce::String (build::productName).toUpperCase(), header.withTrimmedLeft (space::l).withWidth (120), juce::Justification::centredLeft);

    // Active tab underline (animated)
    if (tabUnderline.initialised)
    {
        g.setColour (colours::accent);
        g.fillRoundedRectangle (tabUnderline.get(), 1.0f);
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
    const auto f = ui::font (type::body, Weight::Medium);
    const int textW = juce::jmin (260, juce::GlyphArrangement::getStringWidthInt (f, text));
    statusPill = juce::Rectangle<int> (getWidth() - space::l - textW - 18, header.getY(), textW + 18, header.getHeight());
    // the dot breathes while a generation is running
    const bool busy = processor.generation().getCurrentJob().state == JobState::Generating
                   || processor.generation().getCurrentJob().state == JobState::Submitting;
    float dotSize = 7.0f;
    if (busy && motion::enabled())
    {
        dot = colours::accent;
        dotSize = 6.0f + 2.0f * (0.5f + 0.5f * std::sin ((float) (motion::nowMs() / 260.0)));
    }
    g.setColour (dot);
    g.fillEllipse ((float) statusPill.getX() + (7.0f - dotSize) * 0.5f, (float) statusPill.getCentreY() - dotSize * 0.5f, dotSize, dotSize);
    g.setColour (conn.ok() || conn.status == ConnectionStatus::Testing ? colours::dim : colours::text);
    g.setFont (f);
    g.drawText (text, statusPill.withTrimmedLeft (14), juce::Justification::centredLeft, true);

    // License: only shown when it needs attention.
    const bool licensed = processor.isLicensed();
    int rightEdge = statusPill.getX() - space::m;
    if (! licensed)
    {
        const auto chipText = LicenseManager::toString (processor.services().license().getStatus());
        const int chipW = juce::GlyphArrangement::getStringWidthInt (f, chipText) + 20;
        licenseChip = juce::Rectangle<int> (rightEdge - chipW, header.getCentreY() - 11, chipW, 22);
        g.setColour (colours::lamp.withAlpha (0.13f));
        g.fillRoundedRectangle (licenseChip.toFloat(), radius::control);
        g.setColour (colours::lamp);
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
    auto tabs = header.withTrimmedLeft (140).withWidth (300);
    for (auto* b : { &generateTabButton, &historyTabButton, &settingsTabButton })
    {
        b->setBounds (tabs.removeFromLeft (84).withSizeKeepingCentre (84, 30));
        tabs.removeFromLeft (2);
    }
    // undo / redo / shortcuts sit just right of the tabs: they act on the page you see
    auto tools = header.withTrimmedLeft (140 + 3 * 86 + ui::space::l).withWidth (3 * 30 + 8);
    for (auto* b : { &undoButton, &redoButton })
        b->setBounds (tools.removeFromLeft (30).withSizeKeepingCentre (28, 28));
    tools.removeFromLeft (8);
    keysButton.setBounds (tools.removeFromLeft (30).withSizeKeepingCentre (28, 28));

    auto* active = currentTab == "history" ? &historyTabButton : currentTab == "settings" ? &settingsTabButton : &generateTabButton;
    tabUnderline.snap ({ (float) active->getX() + 10.0f, (float) ui::height::topBar - 2.0f, (float) active->getWidth() - 20.0f, 2.0f });

    workspace.setBounds (r);
    historyTab.setBounds (r);
    settingsTab.setBounds (r);
    if (onboarding != nullptr)
        onboarding->setBounds (getLocalBounds());
    if (sheet != nullptr)
        sheet->setBounds (getLocalBounds());
}
} // namespace context
