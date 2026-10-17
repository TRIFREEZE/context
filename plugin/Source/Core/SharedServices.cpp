#include "SharedServices.h"
#include "AppPaths.h"
#include "BuildInfo.h"
#include "Licensing/DeviceId.h"
#include "Storage/CacheManager.h"

namespace context
{
SharedServices::SharedServices()
{
    log = std::make_unique<ContextLogger> (AppPaths::logsDir());
    worker.onTaskException = [] (const juce::String& w) { logging::error ("files", "File task threw: " + w); };

    settingsStore = std::make_unique<SettingsStore> (AppPaths::settingsFile(), worker);
    applyLogLevel();

    logging::info ("startup", juce::String (build::productName) + " " + build::version + " (" + build::gitCommit + ", "
                              + build::buildConfig + ") on " + juce::SystemStats::getOperatingSystemName());
    if (settingsStore->wasRecoveredFromCorruption())
        logging::warning ("startup", "Settings were reset because settings.json was corrupt");

    historyStore = std::make_unique<HistoryStore> (AppPaths::historyFile(), worker);

    LicenseManager::Config lc;
    lc.stateFile = AppPaths::licenseFile();
    lc.publicKeyBase64 = build::licensePublicKey;
    lc.serverUrl = [this] { return settingsStore->get().licenseServerUrl; };
    lc.pluginVersion = build::version;
    lc.deviceId = DeviceId::get();
    licenseManager = std::make_unique<LicenseManager> (lc);

    // Housekeeping off the message thread.
    worker.post ([this]
    {
        CacheManager::cleanTemp (tempFolder(), juce::RelativeTime::hours (1));
        const int missing = CacheManager::pruneMissing (*historyStore);
        if (missing > 0)
            logging::info ("history", "Removed " + juce::String (missing) + " entries whose files were deleted outside Context");
    });
}

SharedServices::~SharedServices()
{
    settingsStore->saveNow();
    historyStore->saveNow();
    worker.waitUntilIdle (5000);
    worker.clearPending();
    licenseManager.reset();
    historyStore.reset();
    settingsStore.reset();
    logging::info ("shutdown", "Services stopped");
    log.reset();
}

void SharedServices::applyLogLevel()
{
    const auto l = settingsStore->get().logLevel;
    log->setLevel (l == "error" ? LogLevel::Error : l == "warning" ? LogLevel::Warning : l == "debug" ? LogLevel::Debug : LogLevel::Info);
}

juce::File SharedServices::generatedFolder() const
{
    const auto custom = settingsStore->get().generatedFolder;
    if (custom.isNotEmpty() && juce::File::isAbsolutePath (custom))
        return juce::File (custom);
    return AppPaths::defaultGeneratedDir();
}

juce::File SharedServices::tempFolder() const { return AppPaths::tempDir(); }

void SharedServices::scheduleCacheMaintenance()
{
    worker.post ([this]
    {
        const auto limit = (juce::int64) settingsStore->get().cacheLimitMB * 1024 * 1024;
        CacheManager::enforceLimit (*historyStore, generatedFolder(), limit, build::productName);
    });
}
} // namespace context
