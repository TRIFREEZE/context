#pragma once

#include <juce_events/juce_events.h>
#include <functional>

namespace context
{
class SerialWorker;

/** User settings shared by every Context instance (settings.json).
    Plain value type: copy it, change it, hand it back to SettingsStore::update. */
struct Settings
{
    // Generation provider
    juce::String providerId          = "bridge";                 // "bridge" | "mock"
    juce::String bridgeUrl           = "http://127.0.0.1:3000";
    juce::String bridgeProfileId     = "context-bridge-v1";
    bool allowNonLocalBridge         = false;
    juce::String acknowledgedNonLocalHost;                       // host the user explicitly accepted
    int generationTimeoutSeconds     = 600;

    // Licensing / updates
    juce::String licenseServerUrl;
    bool checkForUpdates             = true;
    juce::int64 lastUpdateCheckMs    = 0;

    // Storage
    juce::String generatedFolder;                                // empty = default managed folder
    int cacheLimitMB                 = 2048;

    // Track context
    int captureSeconds               = 8;
    bool useTrackContext             = true;
    bool promptExpansion             = true;
    bool allowSendAudioContext       = false;

    // Preview
    float previewGainDb              = -6.0f;

    // Diagnostics / developer
    juce::String logLevel            = "info";                   // error | warning | info | debug
    bool developerMode               = false;
    double mockLatencyScale          = 1.0;
    int mockFailurePercent           = 0;

    // UI
    bool firstRunComplete            = false;
    bool showEnhancedPrompt          = true;

    static Settings defaults();
    juce::var toVar() const;
    /** Reads known keys, ignores unknown ones, clamps every value into range. */
    static Settings fromVar (const juce::var& v);
    void clamp();

    bool operator== (const Settings&) const = default;
};

/** Thread-safe owner of the current Settings. Saves asynchronously and atomically. */
class SettingsStore : public juce::ChangeBroadcaster
{
public:
    SettingsStore (juce::File file, SerialWorker& fileWorker);

    Settings get() const;
    void update (const std::function<void (Settings&)>& change);
    void reloadFromDisk();

    /** True if the file was unreadable at load time and defaults were used. */
    bool wasRecoveredFromCorruption() const { return recovered; }

    /** Synchronous save (used at shutdown and in tests). */
    bool saveNow();

private:
    void scheduleSave();

    juce::File file;
    SerialWorker& worker;
    mutable juce::CriticalSection lock;
    Settings current;
    bool recovered = false;
};
} // namespace context
