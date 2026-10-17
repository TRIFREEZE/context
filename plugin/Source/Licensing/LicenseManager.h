#pragma once

#include "LicenseToken.h"
#include "Core/SerialWorker.h"
#include "Utils/CancellationToken.h"
#include <juce_events/juce_events.h>
#include <atomic>

namespace context
{
/** Context's own commercial licensing (one instance per process).

    - Activation and validation talk to the Context licensing backend on the
      licensing worker thread. Only the license key, hashed device ID and plugin
      version are sent.
    - Tokens are verified offline with the compiled-in Ed25519 public key.
    - 7-day offline grace: a valid token keeps the plugin licensed even if the
      server is unreachable, until the token's offline_until.
    - Licensing never touches audio. Unlicensed = generation disabled only. */
class LicenseManager : public juce::ChangeBroadcaster
{
public:
    enum class Status
    {
        Unlicensed,        // no token
        Activating,        // request in flight
        Licensed,          // token valid, refreshed recently
        Grace,             // token valid but refresh overdue (server unreachable)
        Expired,           // offline grace ran out: reconnect to continue
        Invalid,           // token rejected (revoked, other device, tampered)
        NotConfigured      // this build has no public key
    };

    struct Config
    {
        juce::File stateFile;                                  // license.json
        juce::String publicKeyBase64;
        std::function<juce::String()> serverUrl;               // from Settings
        juce::String pluginVersion;
        juce::String deviceId;
        std::function<juce::Time()> clock;                     // injectable for tests
    };

    explicit LicenseManager (Config);
    ~LicenseManager() override;

    /** Fast, thread-safe, no I/O. True when generation may run. */
    bool isLicensed() const;

    Status getStatus() const;
    juce::String getMessage() const;              // friendly status / last error
    juce::String getMaskedKey() const;            // CTX-****-****-****-ABCD
    std::optional<juce::Time> getOfflineUntil() const;

    void activate (const juce::String& licenseKey); // async
    void refreshIfDue (bool force = false);         // async
    void deactivate();                              // async: server + local
    void checkForUpdate();                          // async; result via getUpdateInfo()

    struct UpdateInfo { bool checked = false; bool available = false; juce::String latestVersion, downloadUrl, error; };
    UpdateInfo getUpdateInfo() const;

    static juce::String toString (Status);
    static bool isNewerVersion (const juce::String& candidate, const juce::String& current);

    /** Tests: wait until the worker is idle. */
    bool waitForIdle (int ms) { return worker.waitUntilIdle (ms); }

private:
    struct Stored { juce::String key, token; };
    void load();
    void save (const Stored&);
    void applyToken (const juce::String& key, const juce::String& token, bool persist);
    void evaluate();                                 // recompute status from the current token + clock
    void setState (Status, juce::String message);
    juce::String serverBase() const;

    Config cfg;
    mutable juce::CriticalSection lock;
    Stored stored;
    std::optional<LicenseToken> token;
    Status status = Status::Unlicensed;
    juce::String message;
    UpdateInfo update;
    CancellationToken::Ptr shutdownToken = CancellationToken::create();   // aborts in-flight requests on destruction
    std::atomic<bool> licensedFlag { false };
    std::atomic<juce::int64> offlineUntilMs { 0 };

    SerialWorker worker { "Context licensing", 8 };
};
} // namespace context
