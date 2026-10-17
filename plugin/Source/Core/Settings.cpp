#include "Settings.h"
#include "SerialWorker.h"
#include "BuildInfo.h"
#include "Utils/AtomicFile.h"
#include "Utils/Json.h"
#include "Storage/Logger.h"

namespace context
{
Settings Settings::defaults()
{
    Settings s;
    s.licenseServerUrl = build::licenseServerUrl;
    if (build::developerBuild)
    {
        s.providerId = "mock";
        s.logLevel = "debug";
        s.developerMode = true;
        s.mockLatencyScale = 0.25;
    }
    return s;
}

void Settings::clamp()
{
    if (providerId != "bridge" && providerId != "mock")
        providerId = "bridge";
    bridgeUrl = bridgeUrl.trim().substring (0, 512);
    bridgeProfileId = bridgeProfileId.trim().substring (0, 64);
    if (bridgeProfileId.isEmpty())
        bridgeProfileId = "context-bridge-v1";
    acknowledgedNonLocalHost = acknowledgedNonLocalHost.trim().toLowerCase().substring (0, 255);
    generationTimeoutSeconds = juce::jlimit (30, 3600, generationTimeoutSeconds);
    licenseServerUrl = licenseServerUrl.trim().substring (0, 512);
    if (licenseServerUrl.isEmpty())
        licenseServerUrl = build::licenseServerUrl;
    generatedFolder = generatedFolder.trim().substring (0, 1024);
    if (generatedFolder.isNotEmpty() && ! juce::File::isAbsolutePath (generatedFolder))
        generatedFolder = {};
    cacheLimitMB = juce::jlimit (100, 100000, cacheLimitMB);
    captureSeconds = juce::jlimit (4, 30, captureSeconds);
    previewGainDb = juce::jlimit (-24.0f, 0.0f, std::isfinite (previewGainDb) ? previewGainDb : -6.0f);
    if (! juce::StringArray { "error", "warning", "info", "debug" }.contains (logLevel))
        logLevel = "info";
    mockLatencyScale = juce::jlimit (0.05, 5.0, std::isfinite (mockLatencyScale) ? mockLatencyScale : 1.0);
    mockFailurePercent = juce::jlimit (0, 100, mockFailurePercent);
}

juce::var Settings::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("version", 1);
    o->setProperty ("providerId", providerId);
    o->setProperty ("bridgeUrl", bridgeUrl);
    o->setProperty ("bridgeProfileId", bridgeProfileId);
    o->setProperty ("allowNonLocalBridge", allowNonLocalBridge);
    o->setProperty ("acknowledgedNonLocalHost", acknowledgedNonLocalHost);
    o->setProperty ("generationTimeoutSeconds", generationTimeoutSeconds);
    o->setProperty ("licenseServerUrl", licenseServerUrl);
    o->setProperty ("checkForUpdates", checkForUpdates);
    o->setProperty ("lastUpdateCheckMs", lastUpdateCheckMs);
    o->setProperty ("generatedFolder", generatedFolder);
    o->setProperty ("cacheLimitMB", cacheLimitMB);
    o->setProperty ("captureSeconds", captureSeconds);
    o->setProperty ("useTrackContext", useTrackContext);
    o->setProperty ("promptExpansion", promptExpansion);
    o->setProperty ("reduceMotion", reduceMotion);
    o->setProperty ("allowSendAudioContext", allowSendAudioContext);
    o->setProperty ("previewGainDb", previewGainDb);
    o->setProperty ("logLevel", logLevel);
    o->setProperty ("developerMode", developerMode);
    o->setProperty ("mockLatencyScale", mockLatencyScale);
    o->setProperty ("mockFailurePercent", mockFailurePercent);
    o->setProperty ("firstRunComplete", firstRunComplete);
    o->setProperty ("showEnhancedPrompt", showEnhancedPrompt);
    return juce::var (o);
}

Settings Settings::fromVar (const juce::var& v)
{
    Settings s = defaults();
    auto str  = [&] (const char* k, juce::String& out) { if (auto x = json::getString (v, k)) out = *x; };
    auto bl   = [&] (const char* k, bool& out)         { if (auto x = json::getBool (v, k)) out = *x; };
    auto num  = [&] (const char* k, auto& out)         { if (auto x = json::getNumber (v, k)) out = (std::remove_reference_t<decltype (out)>) *x; };

    str ("providerId", s.providerId);
    str ("bridgeUrl", s.bridgeUrl);
    str ("bridgeProfileId", s.bridgeProfileId);
    bl  ("allowNonLocalBridge", s.allowNonLocalBridge);
    str ("acknowledgedNonLocalHost", s.acknowledgedNonLocalHost);
    num ("generationTimeoutSeconds", s.generationTimeoutSeconds);
    str ("licenseServerUrl", s.licenseServerUrl);
    bl  ("checkForUpdates", s.checkForUpdates);
    num ("lastUpdateCheckMs", s.lastUpdateCheckMs);
    str ("generatedFolder", s.generatedFolder);
    num ("cacheLimitMB", s.cacheLimitMB);
    num ("captureSeconds", s.captureSeconds);
    bl  ("useTrackContext", s.useTrackContext);
    bl  ("promptExpansion", s.promptExpansion);
    bl  ("reduceMotion", s.reduceMotion);
    bl  ("allowSendAudioContext", s.allowSendAudioContext);
    num ("previewGainDb", s.previewGainDb);
    str ("logLevel", s.logLevel);
    bl  ("developerMode", s.developerMode);
    num ("mockLatencyScale", s.mockLatencyScale);
    num ("mockFailurePercent", s.mockFailurePercent);
    bl  ("firstRunComplete", s.firstRunComplete);
    bl  ("showEnhancedPrompt", s.showEnhancedPrompt);
    s.clamp();
    return s;
}

// ---------------------------------------------------------------------------

SettingsStore::SettingsStore (juce::File f, SerialWorker& w)
    : file (std::move (f)), worker (w)
{
    current = Settings::defaults();
    reloadFromDisk();
}

SettingsStore::~SettingsStore()
{
    worker.waitUntilIdle (10000);
}

void SettingsStore::reloadFromDisk()
{
    Settings loaded = Settings::defaults();
    bool wasCorrupt = false;

    if (file.existsAsFile())
    {
        const auto text = file.getSize() < 256 * 1024 ? file.loadFileAsString() : juce::String();
        const auto parsed = json::parseUntrusted (text, 256 * 1024, 16);
        if (parsed.ok && json::isObject (parsed.value))
        {
            loaded = Settings::fromVar (parsed.value);
        }
        else
        {
            wasCorrupt = true;
            const auto backup = file.getSiblingFile ("settings.corrupt-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".json");
            file.moveFileTo (backup);
            logging::warning ("settings", "settings.json was unreadable (" + parsed.error + "); using defaults. Backup: " + backup.getFileName());
        }
    }

    {
        const juce::ScopedLock sl (lock);
        current = loaded;
        recovered = wasCorrupt;
    }
    sendChangeMessage();
}

Settings SettingsStore::get() const
{
    const juce::ScopedLock sl (lock);
    return current;
}

void SettingsStore::update (const std::function<void (Settings&)>& change)
{
    bool changed = false;
    {
        const juce::ScopedLock sl (lock);
        auto copy = current;
        change (copy);
        copy.clamp();
        changed = ! (copy == current);
        current = copy;
    }
    if (changed)
    {
        scheduleSave();
        sendChangeMessage();
    }
}

bool SettingsStore::saveNow()
{
    const auto text = juce::JSON::toString (get().toVar());
    return atomicfile::writeText (file, text);
}

void SettingsStore::scheduleSave()
{
    if (! worker.post ([this] { saveNow(); }))
        saveNow();
}
} // namespace context
