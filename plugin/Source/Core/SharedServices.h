#pragma once

#include "SerialWorker.h"
#include "Settings.h"
#include "Storage/Logger.h"
#include "Storage/HistoryStore.h"
#include "Licensing/LicenseManager.h"

namespace context
{
/** Process-wide services shared by every Context instance, created on first use
    through juce::SharedResourcePointer<SharedServices> and destroyed with the last
    instance. Construction is cheap (small file reads only, no network). */
class SharedServices
{
public:
    SharedServices();
    ~SharedServices();

    SerialWorker& fileWorker()      { return worker; }
    ContextLogger& logger()         { return *log; }
    SettingsStore& settings()       { return *settingsStore; }
    HistoryStore& history()         { return *historyStore; }
    LicenseManager& license()       { return *licenseManager; }

    /** Managed folder for generated audio (setting or %APPDATA%\Context\Generated). */
    juce::File generatedFolder() const;
    juce::File tempFolder() const;

    /** Runs cache-limit enforcement on the file worker. */
    void scheduleCacheMaintenance();

    void applyLogLevel();

private:
    SerialWorker worker { "Context files", 256 };     // declared first: destroyed last
    std::unique_ptr<ContextLogger> log;
    std::unique_ptr<SettingsStore> settingsStore;
    std::unique_ptr<HistoryStore> historyStore;
    std::unique_ptr<LicenseManager> licenseManager;

    JUCE_DECLARE_NON_COPYABLE (SharedServices)
};
} // namespace context
