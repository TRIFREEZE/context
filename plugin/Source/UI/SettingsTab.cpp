#include "SettingsTab.h"
#include "Theme.h"
#include "PluginProcessor.h"
#include "BuildInfo.h"
#include "Core/AppPaths.h"
#include "Generation/BridgeConfiguration.h"
#include "Storage/CacheManager.h"
#include "Utils/UrlSafety.h"

namespace context::ui
{
namespace
{
    juce::Colour statusColour (ConnectionStatus s)
    {
        switch (s)
        {
            case ConnectionStatus::Connected:
            case ConnectionStatus::MockReady:   return colours::good;
            case ConnectionStatus::Testing:
            case ConnectionStatus::Unknown:     return colours::dim;
            default:                            return colours::bad;
        }
    }

    void styleEditor (juce::TextEditor& e)
    {
        e.setFont (ContextLookAndFeel::font (14.0f));
        e.setIndents (8, 6);
    }

    void styleSlider (juce::Slider& s, const juce::String& suffix)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 22);
        s.setTextValueSuffix (suffix);
    }
}

class SettingsTab::Content : public juce::Component
{
public:
    explicit Content (SettingsTab& o, ContextAudioProcessor& p) : owner (o), processor (p)
    {
        auto& settings = processor.services().settings();

        // ---------------- License ----------------
        styleEditor (licenseKey);
        licenseKey.setTextToShowWhenEmpty ("CTX-XXXX-XXXX-XXXX-XXXX", colours::faint);
        licenseKey.setInputRestrictions (40);
        licenseKey.onReturnKey = [this] { activate(); };
        stylePrimary (activateButton);
        activateButton.onClick = [this] { activate(); };
        deactivateButton.onClick = [this]
        {
            juce::Component::SafePointer<Content> safe (this);
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                              .withTitle ("Deactivate Context on this computer?")
                                              .withMessage ("This frees a device slot on your license. Generation stops until you activate again. Audio passthrough is unaffected.")
                                              .withButton ("Deactivate").withButton ("Cancel").withAssociatedComponent (this),
                                          [safe] (int r) { if (r == 1 && safe != nullptr) safe->processor.services().license().deactivate(); });
        };
        styleEditor (licenseServer);
        licenseServer.onReturnKey = [this] { commitLicenseServer(); };
        licenseServer.onFocusLost = [this] { commitLicenseServer(); };

        // ---------------- Generation ----------------
        provider.addItem ("Local Generation Bridge", 1);
        provider.addItem ("Mock Provider (offline test, no AI)", 2);
        provider.onChange = [this, &settings]
        {
            const auto id = provider.getSelectedId() == 2 ? "mock" : "bridge";
            settings.update ([id] (Settings& s) { s.providerId = id; });
            processor.generation().testConnection();
        };

        styleEditor (bridgeUrl);
        bridgeUrl.setTextToShowWhenEmpty ("http://127.0.0.1:3000", colours::faint);
        bridgeUrl.onReturnKey = [this] { commitBridgeUrl(); processor.generation().testConnection(); };
        bridgeUrl.onFocusLost = [this] { commitBridgeUrl(); };
        stylePrimary (testButton);
        testButton.onClick = [this] { commitBridgeUrl(); processor.rebuildProvider(); processor.generation().testConnection(); };

        profile.onChange = [this, &settings]
        {
            const auto idx = profile.getSelectedItemIndex();
            if (idx >= 0 && idx < profileIds.size())
            {
                const auto id = profileIds[idx];
                settings.update ([id] (Settings& s) { s.bridgeProfileId = id; });
            }
        };
        editProfilesButton.setTooltip ("Open the folder that contains bridge_profiles.json (configuration only, never code)");
        editProfilesButton.onClick = [this]
        {
            auto f = AppPaths::bridgeProfilesFile();
            if (! f.existsAsFile())
                f.replaceWithText (exampleProfiles());
            f.revealToUser();
        };

        allowNonLocal.onClick = [this] { toggleNonLocal(); };

        styleSlider (timeout, " s");
        timeout.setRange (30, 1800, 10);
        timeout.onDragEnd = [this, &settings] { const int v = (int) timeout.getValue(); settings.update ([v] (Settings& s) { s.generationTimeoutSeconds = v; }); };

        // ---------------- Track context ----------------
        styleSlider (captureSeconds, " s");
        captureSeconds.setRange (4, 30, 1);
        captureSeconds.onDragEnd = [this, &settings] { const int v = (int) captureSeconds.getValue(); settings.update ([v] (Settings& s) { s.captureSeconds = v; }); };
        promptExpansion.onClick = [this, &settings] { const bool v = promptExpansion.getToggleState(); settings.update ([v] (Settings& s) { s.promptExpansion = v; }); };
        showEnhanced.onClick = [this, &settings] { const bool v = showEnhanced.getToggleState(); settings.update ([v] (Settings& s) { s.showEnhancedPrompt = v; }); };
        allowAudioContext.onClick = [this] { toggleAudioContext(); };

        // ---------------- Storage ----------------
        folderLabel.setFont (ContextLookAndFeel::font (13.0f));
        folderLabel.setColour (juce::Label::textColourId, colours::dim);
        changeFolderButton.onClick = [this] { chooseFolder(); };
        openFolderButton.onClick = [this] { auto f = processor.services().generatedFolder(); f.createDirectory(); f.revealToUser(); };
        styleSlider (cacheLimit, " MB");
        cacheLimit.setRange (200, 20000, 100);
        cacheLimit.onDragEnd = [this, &settings]
        {
            const int v = (int) cacheLimit.getValue();
            settings.update ([v] (Settings& s) { s.cacheLimitMB = v; });
            processor.services().scheduleCacheMaintenance();
        };
        clearCacheButton.onClick = [this] { clearCache(); };

        // ---------------- Preview ----------------
        styleSlider (previewGain, " dB");
        previewGain.setRange (-24.0, 0.0, 0.5);
        previewGain.onValueChange = [this, &settings] { const float v = (float) previewGain.getValue(); settings.update ([v] (Settings& s) { s.previewGainDb = v; }); };

        // ---------------- Diagnostics ----------------
        logLevel.addItemList ({ "Errors only", "Warnings", "Normal", "Verbose (debug)" }, 1);
        logLevel.onChange = [this, &settings]
        {
            static const char* ids[] = { "error", "warning", "info", "debug" };
            const auto v = juce::String (ids[juce::jlimit (0, 3, logLevel.getSelectedItemIndex())]);
            settings.update ([v] (Settings& s) { s.logLevel = v; });
        };
        copyDiagnostics.onClick = [this]
        {
            juce::SystemClipboard::copyTextToClipboard (processor.diagnosticsText());
            diagnosticsHint.setText ("Diagnostic info copied. Paste it into your message to support or Claude.", juce::dontSendNotification);
        };
        openLogs.onClick = [this] { processor.services().logger().getLogDirectory().revealToUser(); };
        developerMode.onClick = [this, &settings] { const bool v = developerMode.getToggleState(); settings.update ([v] (Settings& s) { s.developerMode = v; }); };
        styleSlider (mockFailure, " %");
        mockFailure.setRange (0, 100, 1);
        mockFailure.onDragEnd = [this, &settings] { const int v = (int) mockFailure.getValue(); settings.update ([v] (Settings& s) { s.mockFailurePercent = v; }); };
        styleSlider (mockSpeed, "x");
        mockSpeed.setRange (0.05, 3.0, 0.05);
        mockSpeed.onDragEnd = [this, &settings] { const double v = mockSpeed.getValue(); settings.update ([v] (Settings& s) { s.mockLatencyScale = v; }); };

        // ---------------- About ----------------
        checkUpdates.onClick = [this] { processor.services().license().checkForUpdate(); updateLabel.setText ("Checking...", juce::dontSendNotification); };
        downloadUpdate.onClick = [this]
        {
            const auto u = processor.services().license().getUpdateInfo();
            if (u.downloadUrl.isNotEmpty())
                juce::URL (u.downloadUrl).launchInDefaultBrowser();
        };
        setupAgain.onClick = [this] { if (owner.onRunSetupAgain) owner.onRunSetupAgain(); };

        for (auto* l : { &licenseStatus, &connectionStatus, &bridgeHint, &cacheUsage, &updateLabel, &aboutLabel, &diagnosticsHint })
        {
            l->setFont (ContextLookAndFeel::font (13.0f));
            l->setColour (juce::Label::textColourId, colours::dim);
            l->setMinimumHorizontalScale (1.0f);
        }
        aboutLabel.setJustificationType (juce::Justification::topLeft);

        for (auto* b : { &deactivateButton, &editProfilesButton, &changeFolderButton, &openFolderButton, &clearCacheButton,
                         &copyDiagnostics, &openLogs, &checkUpdates, &downloadUpdate, &setupAgain })
            styleQuiet (*b);

        for (juce::Component* c : std::initializer_list<juce::Component*> {
                 &licenseStatus, &licenseKey, &activateButton, &deactivateButton, &licenseServer,
                 &provider, &bridgeUrl, &testButton, &connectionStatus, &profile, &editProfilesButton, &allowNonLocal, &bridgeHint, &timeout,
                 &captureSeconds, &promptExpansion, &showEnhanced, &allowAudioContext,
                 &folderLabel, &changeFolderButton, &openFolderButton, &cacheLimit, &cacheUsage, &clearCacheButton,
                 &previewGain,
                 &logLevel, &copyDiagnostics, &openLogs, &diagnosticsHint, &developerMode, &mockFailure, &mockSpeed,
                 &aboutLabel, &updateLabel, &checkUpdates, &downloadUpdate, &setupAgain })
            addAndMakeVisible (c);
    }

    // ---------------------------------------------------------------- actions
    void activate()
    {
        commitLicenseServer();
        processor.services().license().activate (licenseKey.getText());
        refresh();
    }

    void commitLicenseServer()
    {
        const auto text = licenseServer.getText().trim();
        if (text.isEmpty() || ! urls::parseBaseUrl (text).ok)
            return;
        processor.services().settings().update ([text] (Settings& s) { s.licenseServerUrl = text; });
    }

    void commitBridgeUrl()
    {
        auto text = bridgeUrl.getText().trim();
        if (text.isEmpty())
            text = "http://127.0.0.1:3000";
        const auto parsed = urls::parseBaseUrl (text);
        if (! parsed.ok)
        {
            bridgeHint.setText ("Invalid address: " + parsed.error, juce::dontSendNotification);
            bridgeHint.setColour (juce::Label::textColourId, colours::bad);
            return;
        }
        processor.services().settings().update ([parsed] (Settings& s)
        {
            s.bridgeUrl = parsed.normalized;
            // A previous "allow" applies only to the host that was acknowledged.
            if (s.allowNonLocalBridge && parsed.kind != urls::HostKind::Loopback && s.acknowledgedNonLocalHost != parsed.host)
            {
                s.allowNonLocalBridge = false;
                s.acknowledgedNonLocalHost = {};
            }
        });
        processor.rebuildProvider();
        refresh();
    }

    void toggleNonLocal()
    {
        auto& settings = processor.services().settings();
        if (! allowNonLocal.getToggleState())
        {
            settings.update ([] (Settings& s) { s.allowNonLocalBridge = false; s.acknowledgedNonLocalHost = {}; });
            processor.rebuildProvider();
            return;
        }
        allowNonLocal.setToggleState (false, juce::dontSendNotification);
        const auto parsed = urls::parseBaseUrl (settings.get().bridgeUrl);
        if (! parsed.ok || parsed.kind == urls::HostKind::Loopback)
        {
            bridgeHint.setText ("The bridge address is already on this computer; nothing to allow.", juce::dontSendNotification);
            return;
        }
        juce::Component::SafePointer<Content> safe (this);
        const auto host = parsed.host;
        const bool insecure = parsed.scheme == "http";
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle ("Use a bridge that is not on this computer?")
                                          .withMessage ("The bridge at " + host + " is on " + urls::describe (parsed.kind) + ".\n\n"
                                                        + "Your prompts" + juce::String (insecure ? " will travel unencrypted (http) over the network" : " will be sent over the network")
                                                        + ", and so will audio context if you enable it. Only continue if you run and trust that bridge.")
                                          .withButton ("I understand, allow it").withButton ("Cancel").withAssociatedComponent (this),
                                      [safe, host] (int r)
        {
            if (r != 1 || safe == nullptr)
                return;
            safe->processor.services().settings().update ([host] (Settings& s) { s.allowNonLocalBridge = true; s.acknowledgedNonLocalHost = host; });
            safe->processor.rebuildProvider();
            safe->processor.generation().testConnection();
            safe->refresh();
        });
    }

    void toggleAudioContext()
    {
        auto& settings = processor.services().settings();
        if (! allowAudioContext.getToggleState())
        {
            settings.update ([] (Settings& s) { s.allowSendAudioContext = false; });
            return;
        }
        allowAudioContext.setToggleState (false, juce::dontSendNotification);
        juce::Component::SafePointer<Content> safe (this);
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle ("Allow sending audio context?")
                                          .withMessage ("When you also tick \"Send Audio Context\" on the Generate screen, Context sends the last few seconds of your track "
                                                        "(mono, 22 kHz) to your generation bridge with the request. Track audio is never sent to the Context licensing server.")
                                          .withButton ("Allow").withButton ("Cancel").withAssociatedComponent (this),
                                      [safe] (int r)
        {
            if (r == 1 && safe != nullptr)
                safe->processor.services().settings().update ([] (Settings& s) { s.allowSendAudioContext = true; });
        });
    }

    void chooseFolder()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose where Context stores generated audio", processor.services().generatedFolder());
        juce::Component::SafePointer<Content> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe] (const juce::FileChooser& fc)
        {
            const auto dir = fc.getResult();
            if (safe == nullptr || dir == juce::File() || ! dir.isDirectory())
                return;
            const auto path = dir.getFullPathName();
            safe->processor.services().settings().update ([path] (Settings& s) { s.generatedFolder = path; });
            safe->refresh();
        });
    }

    void clearCache()
    {
        juce::Component::SafePointer<Content> safe (this);
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle ("Clear the cache?")
                                          .withMessage ("Deletes generated sounds in Context's cache folder.\n\nKept: favorites, sounds you dragged into a project, "
                                                        "copied the path of, or opened in Explorer, and every copy you saved elsewhere.")
                                          .withButton ("Clear Cache").withButton ("Cancel").withAssociatedComponent (this),
                                      [safe] (int r)
        {
            if (r != 1 || safe == nullptr)
                return;
            auto& services = safe->processor.services();
            auto* servicesPtr = &services;
            services.fileWorker().post ([servicesPtr, safe]
            {
                const auto rep = CacheManager::clear (servicesPtr->history(), servicesPtr->generatedFolder(), build::productName);
                juce::MessageManager::callAsync ([safe, rep]
                {
                    if (safe != nullptr)
                        safe->cacheUsage.setText ("Removed " + juce::String (rep.deletedFiles) + " file(s), freed "
                                                  + juce::String (rep.freedBytes / (1024.0 * 1024.0), 1) + " MB.", juce::dontSendNotification);
                });
            });
        });
    }

    static juce::String exampleProfiles()
    {
        return R"json({
  "_comment": "Context bridge profiles: CONFIGURATION ONLY (paths, field names, mappings). No code, hosts or credentials. See docs/BRIDGE_PROFILES.md.",
  "profiles": [
    {
      "id": "my-bridge",
      "name": "My bridge (edit me)",
      "requestFormat": "fieldmap",
      "healthPath": "/health",
      "generatePath": "/generate",
      "statusPath": "/jobs/{id}",
      "cancelPath": "",
      "supportsCancellation": false,
      "requestFields": { "prompt": "prompt", "bpm": "bpm" },
      "response": {
        "jobIdPath": "id",
        "statusPath": "status",
        "progressPath": "progress",
        "audioUrlPath": "audio_url",
        "errorPath": "error",
        "statusMap": { "queued": "queued", "running": "generating", "done": "complete", "error": "failed" }
      }
    }
  ]
}
)json";
    }

    // ---------------------------------------------------------------- refresh / layout
    void refresh()
    {
        const auto s = processor.services().settings().get();
        auto& lic = processor.services().license();

        // License
        const auto st = lic.getStatus();
        licenseStatus.setText (LicenseManager::toString (st) + ". " + lic.getMessage(), juce::dontSendNotification);
        licenseStatus.setColour (juce::Label::textColourId, lic.isLicensed() ? colours::good : st == LicenseManager::Status::Activating ? colours::dim : colours::warn);
        activateButton.setEnabled (st != LicenseManager::Status::Activating && st != LicenseManager::Status::NotConfigured);
        deactivateButton.setEnabled (lic.getMaskedKey().isNotEmpty());
        if (! licenseServer.hasKeyboardFocus (true))
            licenseServer.setText (s.licenseServerUrl, juce::dontSendNotification);

        // Generation
        provider.setSelectedId (s.providerId == "mock" ? 2 : 1, juce::dontSendNotification);
        const bool bridge = s.providerId == "bridge";
        if (! bridgeUrl.hasKeyboardFocus (true))
            bridgeUrl.setText (s.bridgeUrl, juce::dontSendNotification);
        for (juce::Component* c : std::initializer_list<juce::Component*> { &bridgeUrl, &profile, &editProfilesButton, &allowNonLocal })
            c->setEnabled (bridge);

        const auto conn = processor.generation().getConnectionResult();
        juce::String connText = toDisplayString (conn.status);
        if (conn.status == ConnectionStatus::Connected)
            connText = glyph ("\xe2\x9c\x93 ") + "Local generation bridge detected" + (conn.bridgeName.isNotEmpty() ? " (" + conn.bridgeName + ")" : juce::String());
        else if (conn.status == ConnectionStatus::ConnectionRefused || conn.status == ConnectionStatus::Disconnected)
            connText = glyph ("\xe2\x9c\x95 ") + "No bridge detected. Start your local generation bridge and click Test Connection again.";
        else if (conn.status != ConnectionStatus::Unknown && conn.status != ConnectionStatus::Testing && ! conn.ok())
            connText = glyph ("\xe2\x9c\x95 ") + connText + (conn.detail.isNotEmpty() ? ": " + conn.detail : juce::String());
        connectionStatus.setText (connText, juce::dontSendNotification);
        connectionStatus.setColour (juce::Label::textColourId, statusColour (conn.status));

        const auto profiles = BridgeProfileSet::load (AppPaths::bridgeProfilesFile());
        profile.clear (juce::dontSendNotification);
        profileIds.clear();
        int selectedIdx = 0;
        for (size_t i = 0; i < profiles.profiles.size(); ++i)
        {
            profile.addItem (profiles.profiles[i].name, (int) i + 1);
            profileIds.add (profiles.profiles[i].id);
            if (profiles.profiles[i].id == s.bridgeProfileId)
                selectedIdx = (int) i;
        }
        profile.setSelectedItemIndex (selectedIdx, juce::dontSendNotification);

        allowNonLocal.setToggleState (s.allowNonLocalBridge, juce::dontSendNotification);
        const auto parsed = urls::parseBaseUrl (s.bridgeUrl);
        juce::String hint;
        if (! profiles.problems.isEmpty())
            hint = "Profile file problems: " + profiles.problems.joinIntoString ("; ");
        else if (parsed.ok && parsed.kind != urls::HostKind::Loopback && ! s.allowNonLocalBridge)
            hint = "This address is on " + urls::describe (parsed.kind) + ". Context only uses local bridges unless you allow it below.";
        else if (parsed.ok && parsed.kind != urls::HostKind::Loopback)
            hint = "Non-local bridge allowed for " + s.acknowledgedNonLocalHost + ".";
        else
            hint = "Context never asks for provider passwords, cookies or tokens. Your bridge handles its own sign-in.";
        bridgeHint.setText (hint, juce::dontSendNotification);
        bridgeHint.setColour (juce::Label::textColourId, profiles.problems.isEmpty() ? colours::dim : colours::warn);
        timeout.setValue (s.generationTimeoutSeconds, juce::dontSendNotification);

        // Track context
        captureSeconds.setValue (s.captureSeconds, juce::dontSendNotification);
        promptExpansion.setToggleState (s.promptExpansion, juce::dontSendNotification);
        showEnhanced.setToggleState (s.showEnhancedPrompt, juce::dontSendNotification);
        allowAudioContext.setToggleState (s.allowSendAudioContext, juce::dontSendNotification);

        // Storage
        folderLabel.setText (processor.services().generatedFolder().getFullPathName(), juce::dontSendNotification);
        cacheLimit.setValue (s.cacheLimitMB, juce::dontSendNotification);
        const auto used = CacheManager::trackedBytes (processor.services().history());
        cacheUsage.setText ("Using " + juce::String (used / (1024.0 * 1024.0), 1) + " MB of " + juce::String (s.cacheLimitMB) + " MB. "
                            "Oldest unused sounds are removed first; favorites never.", juce::dontSendNotification);

        previewGain.setValue (s.previewGainDb, juce::dontSendNotification);

        // Diagnostics
        const juce::StringArray levels { "error", "warning", "info", "debug" };
        logLevel.setSelectedItemIndex (juce::jmax (0, levels.indexOf (s.logLevel)), juce::dontSendNotification);
        developerMode.setToggleState (s.developerMode, juce::dontSendNotification);
        mockFailure.setValue (s.mockFailurePercent, juce::dontSendNotification);
        mockSpeed.setValue (s.mockLatencyScale, juce::dontSendNotification);
        mockFailure.setVisible (s.developerMode);
        mockSpeed.setVisible (s.developerMode);

        // About
        aboutLabel.setText (juce::String (build::productName) + "  Version " + build::version + "  " + glyph ("\xc2\xb7") + "  Build " + build::gitCommit
                            + "  " + glyph ("\xc2\xb7") + "  " + build::buildConfig + "\nJUCE " + juce::SystemStats::getJUCEVersion().fromFirstOccurrenceOf ("v", false, false)
                            + "  " + glyph ("\xc2\xb7") + "  Data folder: " + AppPaths::root().getFullPathName(), juce::dontSendNotification);
        const auto u = lic.getUpdateInfo();
        updateLabel.setText (! u.checked ? "Updates are never installed automatically."
                             : u.error.isNotEmpty() ? u.error
                             : u.available ? "Version " + u.latestVersion + " available"
                             : "You're up to date.", juce::dontSendNotification);
        updateLabel.setColour (juce::Label::textColourId, u.available ? colours::accent : colours::dim);
        downloadUpdate.setVisible (u.available && u.downloadUrl.isNotEmpty());

        resized();
    }

    struct Row { juce::String label; std::vector<juce::Component*> comps; int height = 32; std::vector<int> widths; };

    void resized() override
    {
        const auto s = processor.services().settings().get();
        sections.clear();
        sections.push_back ({ "License", {
            { "Status", { &licenseStatus }, 32, {} },
            { "License key", { &licenseKey, &activateButton, &deactivateButton }, 32, { 0, 110, 110 } },
            { "Licensing server", { &licenseServer }, 32, {} } } });
        sections.push_back ({ "Generation", {
            { "Provider", { &provider }, 32, {} },
            { "Bridge address", { &bridgeUrl, &testButton }, 32, { 0, 150 } },
            { "", { &connectionStatus }, 26, {} },
            { "Bridge profile", { &profile, &editProfilesButton }, 32, { 0, 150 } },
            { "", { &allowNonLocal }, 28, {} },
            { "", { &bridgeHint }, 26, {} },
            { "Timeout", { &timeout }, 32, {} } } });
        sections.push_back ({ "Track context", {
            { "Listen buffer", { &captureSeconds }, 32, {} },
            { "", { &promptExpansion }, 28, {} },
            { "", { &showEnhanced }, 28, {} },
            { "", { &allowAudioContext }, 28, {} } } });
        sections.push_back ({ "Storage", {
            { "Generated audio", { &folderLabel, &changeFolderButton, &openFolderButton }, 32, { 0, 100, 100 } },
            { "Cache limit", { &cacheLimit }, 32, {} },
            { "", { &cacheUsage, &clearCacheButton }, 32, { 0, 120 } } } });
        sections.push_back ({ "Preview", { { "Preview volume", { &previewGain }, 32, {} } } });
        std::vector<Row> diag { { "Log level", { &logLevel }, 32, {} },
                                { "", { &copyDiagnostics, &openLogs }, 32, { 200, 160 } },
                                { "", { &diagnosticsHint }, 24, {} },
                                { "", { &developerMode }, 28, {} } };
        if (s.developerMode)
        {
            diag.push_back ({ "Mock failures", { &mockFailure }, 32, {} });
            diag.push_back ({ "Mock speed", { &mockSpeed }, 32, {} });
        }
        sections.push_back ({ "Diagnostics", diag });
        sections.push_back ({ "About", {
            { "", { &aboutLabel }, 40, {} },
            { "", { &updateLabel, &checkUpdates, &downloadUpdate }, 32, { 0, 150, 130 } },
            { "", { &setupAgain }, 32, { 170 } } } });

        const int labelW = 140, margin = 18;
        int y = 10;
        headings.clear();
        rowLabels.clear();
        cards.clear();
        for (auto& sec : sections)
        {
            headings.push_back ({ sec.title, juce::Rectangle<int> (margin, y, 300, 20) });
            y += 26;
            const int cardTop = y - 4;
            for (auto& row : sec.rows)
            {
                auto r = juce::Rectangle<int> (margin + 12, y, getWidth() - 2 * margin - 24, row.height - 4);
                rowLabels.push_back ({ row.label, r.withWidth (labelW) });
                auto content = r.withTrimmedLeft (labelW);
                // Fixed-width components from the right, flexible (0) component takes the rest.
                std::vector<int> widths = row.widths;
                widths.resize (row.comps.size(), 0);
                int fixed = 0, flexCount = 0;
                for (auto w : widths) { fixed += w; flexCount += w == 0 ? 1 : 0; }
                const int gap = 8;
                const int flexW = flexCount > 0 ? juce::jmax (60, (content.getWidth() - fixed - gap * ((int) row.comps.size() - 1)) / flexCount) : 0;
                for (size_t i = 0; i < row.comps.size(); ++i)
                {
                    const int w = widths[i] == 0 ? flexW : widths[i];
                    row.comps[i]->setBounds (content.removeFromLeft (w));
                    content.removeFromLeft (gap);
                }
                y += row.height;
            }
            cards.push_back (juce::Rectangle<int> (margin, cardTop, getWidth() - 2 * margin, y - cardTop + 4));
            y += 18;
        }
        totalHeight = y;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::background);
        for (auto& c : cards)
            paintCard (g, c.toFloat());
        for (auto& h : headings)
            paintHeading (g, h.first, h.second);
        g.setFont (ContextLookAndFeel::font (13.0f));
        g.setColour (colours::dim);
        for (auto& l : rowLabels)
            if (l.first.isNotEmpty())
                g.drawText (l.first, l.second, juce::Justification::centredLeft);
    }

    int getTotalHeight() const { return totalHeight; }

private:
    struct Section { juce::String title; std::vector<Row> rows; };
    SettingsTab& owner;
    ContextAudioProcessor& processor;
    std::vector<Section> sections;
    std::vector<std::pair<juce::String, juce::Rectangle<int>>> headings, rowLabels;
    std::vector<juce::Rectangle<int>> cards;
    int totalHeight = 1200;
    juce::StringArray profileIds;
    std::unique_ptr<juce::FileChooser> chooser;

public:
    juce::Label licenseStatus;
    juce::TextEditor licenseKey, licenseServer, bridgeUrl;
    juce::TextButton activateButton { "Activate" }, deactivateButton { "Deactivate" }, testButton { "Test Connection" },
                     editProfilesButton { "Edit Profiles..." }, changeFolderButton { "Change..." }, openFolderButton { "Open" },
                     clearCacheButton { "Clear Cache" }, copyDiagnostics { "Copy Diagnostic Info" }, openLogs { "Open Logs Folder" },
                     checkUpdates { "Check for Updates" }, downloadUpdate { "Get Update" }, setupAgain { "Run Setup Again" };
    juce::ComboBox provider, profile, logLevel;
    juce::Label connectionStatus, bridgeHint, folderLabel, cacheUsage, updateLabel, aboutLabel, diagnosticsHint;
    juce::ToggleButton allowNonLocal { "Allow non-local generation bridge (another computer / network)" },
                       promptExpansion { "Enhance prompts with track context" },
                       showEnhanced { "Show the enhanced prompt on the Generate screen" },
                       allowAudioContext { "Allow sending audio context to the generation bridge (off by default)" },
                       developerMode { "Developer mode (mock provider controls)" };
    juce::Slider timeout, captureSeconds, cacheLimit, previewGain, mockFailure, mockSpeed;
};

SettingsTab::SettingsTab (ContextAudioProcessor& p)
{
    content = std::make_unique<Content> (*this, p);
    viewport.setViewedComponent (content.get(), false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);
    refresh();
}

SettingsTab::~SettingsTab() = default;

void SettingsTab::refresh()
{
    content->refresh();
    content->setSize (viewport.getMaximumVisibleWidth() > 0 ? viewport.getMaximumVisibleWidth() : getWidth(), content->getTotalHeight());
}

void SettingsTab::resized()
{
    viewport.setBounds (getLocalBounds());
    content->setSize (viewport.getMaximumVisibleWidth(), 100);
    content->resized();
    content->setSize (viewport.getMaximumVisibleWidth(), content->getTotalHeight());
}

void SettingsTab::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}
} // namespace context::ui
