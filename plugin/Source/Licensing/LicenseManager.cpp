#include "LicenseManager.h"
#include "Networking/HttpClient.h"
#include "Storage/Logger.h"
#include "Utils/AtomicFile.h"
#include "Utils/Json.h"
#include "Utils/Redaction.h"
#include "Utils/UrlSafety.h"

namespace context
{
namespace
{
    juce::String normaliseKey (const juce::String& raw)
    {
        auto k = raw.trim().toUpperCase().removeCharacters (" \t\r\n");
        if (k.length() == 19 && ! k.startsWith ("CTX-"))
            k = "CTX-" + k;
        return k;
    }

    bool looksLikeKey (const juce::String& k)
    {
        if (k.length() != 23 || ! k.startsWith ("CTX-"))
            return false;
        for (int i = 4; i < 23; ++i)
        {
            const auto c = k[i];
            if ((i - 4) % 5 == 4) { if (c != '-') return false; }
            else if (! juce::CharacterFunctions::isLetterOrDigit (c)) return false;
        }
        return true;
    }

    juce::String serverMessage (const HttpClient::Response& r, const juce::String& fallback)
    {
        const auto parsed = json::parseUntrusted (r.bodyText(), 64 * 1024, 8);
        if (parsed.ok)
            if (auto m = json::getString (parsed.value, "message"))
                return redaction::redact (m->substring (0, 200));
        return fallback;
    }

    juce::String serverCode (const HttpClient::Response& r)
    {
        const auto parsed = json::parseUntrusted (r.bodyText(), 64 * 1024, 8);
        return parsed.ok ? json::getString (parsed.value, "error").value_or ("") : juce::String();
    }
}

LicenseManager::LicenseManager (Config c) : cfg (std::move (c))
{
    if (! cfg.clock)
        cfg.clock = [] { return juce::Time::getCurrentTime(); };
    worker.onTaskException = [] (const juce::String& w) { logging::error ("license", "Licensing task threw: " + w); };
    load();
}

LicenseManager::~LicenseManager()
{
    worker.clearPending();
    shutdownToken->cancel();   // never hold up DAW shutdown waiting for the licensing server
}

juce::String LicenseManager::toString (Status s)
{
    switch (s)
    {
        case Status::Unlicensed:    return "Not activated";
        case Status::Activating:    return "Activating...";
        case Status::Licensed:      return "Licensed";
        case Status::Grace:         return "Licensed (offline)";
        case Status::Expired:       return "Offline too long";
        case Status::Invalid:       return "License problem";
        case Status::NotConfigured: return "Licensing not configured";
    }
    return "Not activated";
}

bool LicenseManager::isLicensed() const
{
    return licensedFlag.load() && cfg.clock().toMilliseconds() < offlineUntilMs.load();
}

LicenseManager::Status LicenseManager::getStatus() const       { const juce::ScopedLock sl (lock); return status; }
juce::String LicenseManager::getMessage() const                { const juce::ScopedLock sl (lock); return message; }
LicenseManager::UpdateInfo LicenseManager::getUpdateInfo() const { const juce::ScopedLock sl (lock); return update; }

juce::String LicenseManager::getMaskedKey() const
{
    const juce::ScopedLock sl (lock);
    return stored.key.isEmpty() ? juce::String() : redaction::maskLicenseKey (stored.key);
}

std::optional<juce::Time> LicenseManager::getOfflineUntil() const
{
    const juce::ScopedLock sl (lock);
    if (token) return token->offlineUntil;
    return std::nullopt;
}

juce::String LicenseManager::serverBase() const
{
    const auto parsed = urls::parseBaseUrl (cfg.serverUrl ? cfg.serverUrl() : juce::String());
    if (! parsed.ok)
        return {};
    // License keys must not travel unencrypted: plain http is allowed only to this computer (development).
    if (parsed.scheme == "http" && parsed.kind != urls::HostKind::Loopback)
        return {};
    return parsed.normalized;
}

void LicenseManager::setState (Status s, juce::String m)
{
    {
        const juce::ScopedLock sl (lock);
        status = s;
        message = std::move (m);
    }
    sendChangeMessage();
}

// ---------------------------------------------------------------------------

void LicenseManager::load()
{
    if (cfg.publicKeyBase64.isEmpty())
    {
        setState (Status::NotConfigured, "This build was compiled without a license public key, so it can't be activated.");
        return;
    }
    if (! cfg.stateFile.existsAsFile() || cfg.stateFile.getSize() > 64 * 1024)
    {
        setState (Status::Unlicensed, "Enter your Context license key to activate.");
        return;
    }
    const auto parsed = json::parseUntrusted (cfg.stateFile.loadFileAsString(), 64 * 1024, 8);
    if (! parsed.ok)
    {
        logging::warning ("license", "license.json unreadable; ignoring it");
        setState (Status::Unlicensed, "Enter your Context license key to activate.");
        return;
    }
    const auto key = json::getString (parsed.value, "licenseKey").value_or ("");
    const auto tok = json::getString (parsed.value, "token").value_or ("");
    {
        const juce::ScopedLock sl (lock);
        stored.key = key;
    }
    if (tok.isNotEmpty())
        applyToken (key, tok, false);
    else
        setState (Status::Unlicensed, "Enter your Context license key to activate.");
}

void LicenseManager::save (const Stored& s)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("version", 1);
    o->setProperty ("licenseKey", s.key);   // stored locally only, in the user's own app-data folder
    o->setProperty ("token", s.token);
    o->setProperty ("savedAt", juce::Time::getCurrentTime().toMilliseconds());
    if (! atomicfile::writeText (cfg.stateFile, juce::JSON::toString (juce::var (o))))
        logging::warning ("license", "Could not save license.json");
}

void LicenseManager::applyToken (const juce::String& key, const juce::String& tok, bool persist)
{
    auto [check, parsed] = LicenseToken::parseAndVerify (tok, cfg.publicKeyBase64, cfg.deviceId);
    if (check != LicenseToken::Check::Valid || ! parsed)
    {
        logging::warning ("license", "Stored token rejected: " + LicenseToken::checkToString (check));
        {
            const juce::ScopedLock sl (lock);
            token.reset();
            stored.token = {};
        }
        licensedFlag = false;
        setState (check == LicenseToken::Check::WrongDevice ? Status::Invalid : Status::Unlicensed,
                  check == LicenseToken::Check::WrongDevice ? "This activation belongs to another computer. Activate again on this one."
                                                            : "Enter your Context license key to activate.");
        return;
    }

    {
        const juce::ScopedLock sl (lock);
        token = parsed;
        stored.key = key;
        stored.token = tok;
    }
    if (persist)
        save ({ key, tok });
    evaluate();
}

void LicenseManager::evaluate()
{
    std::optional<LicenseToken> t;
    {
        const juce::ScopedLock sl (lock);
        t = token;
    }
    if (! t)
    {
        licensedFlag = false;
        offlineUntilMs = 0;
        return;
    }

    const auto now = cfg.clock();
    const auto check = t->checkTime (now);
    offlineUntilMs = t->offlineUntil.toMilliseconds();

    if (check == LicenseToken::Check::Valid)
    {
        licensedFlag = true;
        if (t->needsRefresh (now))
            setState (Status::Grace, "Licensed. Couldn't reach the licensing server recently; works offline until "
                                     + t->offlineUntil.toString (true, false) + ".");
        else
            setState (Status::Licensed, "Licensed (" + getMaskedKey() + ")");
    }
    else
    {
        licensedFlag = false;
        setState (check == LicenseToken::Check::Expired ? Status::Expired : Status::Invalid,
                  check == LicenseToken::Check::Expired
                      ? "Context has been offline for more than 7 days. Connect to the internet to keep generating. Audio passthrough is unaffected."
                      : "Your Context license could not be validated: " + LicenseToken::checkToString (check) + ".");
    }
}

// ---------------------------------------------------------------------------

void LicenseManager::activate (const juce::String& rawKey)
{
    const auto key = normaliseKey (rawKey);
    if (cfg.publicKeyBase64.isEmpty())
    {
        setState (Status::NotConfigured, "This build was compiled without a license public key, so it can't be activated.");
        return;
    }
    if (! looksLikeKey (key))
    {
        setState (Status::Unlicensed, "License keys look like CTX-XXXX-XXXX-XXXX-XXXX.");
        return;
    }
    const auto base = serverBase();
    if (base.isEmpty())
    {
        setState (Status::Unlicensed, "The licensing server address in Settings is not valid. It must start with https:// "
                                      "(plain http is only allowed for 127.0.0.1 / localhost while testing).");
        return;
    }

    setState (Status::Activating, "Activating...");
    worker.post ([this, key, base]
    {
        HttpClient::Request req;
        req.url = base + "/licenses/activate";
        req.method = "POST";
        req.timeoutMs = 15000;
        req.maxResponseBytes = 64 * 1024;
        auto* o = new juce::DynamicObject();
        o->setProperty ("license_key", key);
        o->setProperty ("device_id", cfg.deviceId);
        o->setProperty ("plugin_version", cfg.pluginVersion);
        req.jsonBody = json::toCompactString (juce::var (o));

        logging::info ("license", "Activating " + redaction::maskLicenseKey (key) + " against " + redaction::safeUrlForDisplay (base));
        const auto r = HttpClient::send (req, *shutdownToken);

        if (! r.connected)
        {
            const bool hadToken = isLicensed();
            if (! hadToken) setState (Status::Unlicensed, "Couldn't reach the Context licensing server. Check your internet connection and the server address in Settings.");
            else evaluate();
            if (auto* l = ContextLogger::getInstance()) l->recordErrorCode ("license_unreachable");
            return;
        }
        if (! r.isSuccess())
        {
            const auto msg = serverMessage (r, "Activation failed (HTTP " + juce::String (r.statusCode) + ").");
            logging::warning ("license", "Activation refused: " + serverCode (r) + " " + msg);
            if (auto* l = ContextLogger::getInstance()) l->recordErrorCode ("license_" + serverCode (r));
            if (isLicensed()) evaluate();
            else setState (Status::Unlicensed, msg);
            return;
        }

        const auto parsed = json::parseUntrusted (r.bodyText(), 64 * 1024, 8);
        const auto tok = parsed.ok ? json::getString (parsed.value, "token").value_or ("") : juce::String();
        auto [check, verified] = LicenseToken::parseAndVerify (tok, cfg.publicKeyBase64, cfg.deviceId);
        if (check != LicenseToken::Check::Valid)
        {
            logging::error ("license", "Server token failed verification: " + LicenseToken::checkToString (check));
            setState (Status::Invalid, "The licensing server's response could not be verified (" + LicenseToken::checkToString (check)
                                       + "). This usually means the plugin and server use different signing keys.");
            return;
        }
        applyToken (key, tok, true);
        logging::info ("license", "Activated; offline grace until " + verified->offlineUntil.toISO8601 (true));
    });
}

void LicenseManager::refreshIfDue (bool force)
{
    std::optional<LicenseToken> t;
    juce::String key;
    {
        const juce::ScopedLock sl (lock);
        t = token;
        key = stored.key;
    }
    evaluate();
    if (! t || key.isEmpty() || (! force && ! t->needsRefresh (cfg.clock())))
        return;

    const auto base = serverBase();
    if (base.isEmpty())
        return;

    worker.post ([this, key, base]
    {
        HttpClient::Request req;
        req.url = base + "/licenses/validate";
        req.method = "POST";
        req.timeoutMs = 10000;
        req.maxResponseBytes = 64 * 1024;
        auto* o = new juce::DynamicObject();
        o->setProperty ("license_key", key);
        o->setProperty ("device_id", cfg.deviceId);
        o->setProperty ("plugin_version", cfg.pluginVersion);
        req.jsonBody = json::toCompactString (juce::var (o));
        const auto r = HttpClient::send (req, *shutdownToken);

        if (! r.connected || r.statusCode >= 500 || r.statusCode == 429)
        {
            // Server unreachable or temporarily failing: keep the current token (offline grace).
            logging::info ("license", "Validation server unreachable (" + juce::String (r.statusCode) + "); using offline grace");
            evaluate();
            return;
        }
        if (! r.isSuccess())
        {
            const auto code = serverCode (r);
            // Definitive refusals end the license on this device.
            if (code == "revoked" || code == "expired" || code == "not_activated" || code == "unknown_license")
            {
                {
                    const juce::ScopedLock sl (lock);
                    token.reset();
                    stored.token = {};
                }
                save ({ key, {} });
                licensedFlag = false;
                setState (Status::Invalid, serverMessage (r, "Your Context license could not be validated."));
                logging::warning ("license", "License refused on validation: " + code);
                return;
            }
            evaluate();
            return;
        }
        const auto parsed = json::parseUntrusted (r.bodyText(), 64 * 1024, 8);
        const auto tok = parsed.ok ? json::getString (parsed.value, "token").value_or ("") : juce::String();
        applyToken (key, tok, true);
    });
}

void LicenseManager::deactivate()
{
    juce::String key;
    {
        const juce::ScopedLock sl (lock);
        key = stored.key;
        token.reset();
        stored = {};
    }
    licensedFlag = false;
    offlineUntilMs = 0;
    cfg.stateFile.deleteFile();
    setState (Status::Unlicensed, "Deactivated on this computer.");

    const auto base = serverBase();
    if (key.isEmpty() || base.isEmpty())
        return;
    worker.post ([this, key, base]
    {
        HttpClient::Request req;
        req.url = base + "/licenses/deactivate";
        req.method = "POST";
        req.timeoutMs = 10000;
        auto* o = new juce::DynamicObject();
        o->setProperty ("license_key", key);
        o->setProperty ("device_id", cfg.deviceId);
        req.jsonBody = json::toCompactString (juce::var (o));
        const auto r = HttpClient::send (req, *shutdownToken);
        if (! r.isSuccess())
            setState (Status::Unlicensed, "Deactivated here, but the server couldn't be reached to free the device slot. "
                                          "It can be reset by the license administrator.");
    });
}

bool LicenseManager::isNewerVersion (const juce::String& candidate, const juce::String& current)
{
    auto parts = [] (const juce::String& v)
    {
        juce::Array<int> p;
        for (auto& s : juce::StringArray::fromTokens (v.trim().trimCharactersAtStart ("vV").upToFirstOccurrenceOf ("-", false, false), ".", ""))
            p.add (s.getIntValue());
        while (p.size() < 3) p.add (0);
        return p;
    };
    const auto a = parts (candidate), b = parts (current);
    for (int i = 0; i < 3; ++i)
        if (a[i] != b[i])
            return a[i] > b[i];
    return false;
}

void LicenseManager::checkForUpdate()
{
    const auto base = serverBase();
    if (base.isEmpty())
        return;
    worker.post ([this, base]
    {
        HttpClient::Request req;
        req.url = base + "/version";
        req.timeoutMs = 8000;
        req.maxResponseBytes = 16 * 1024;
        const auto r = HttpClient::send (req, *shutdownToken);
        UpdateInfo info;
        info.checked = true;
        if (! r.isSuccess())
        {
            info.error = r.connected ? "Update server returned HTTP " + juce::String (r.statusCode) : "Update server unreachable";
        }
        else
        {
            const auto parsed = json::parseUntrusted (r.bodyText(), 16 * 1024, 8);
            info.latestVersion = parsed.ok ? json::getString (parsed.value, "latest_version").value_or ("").substring (0, 32) : juce::String();
            const auto url = parsed.ok ? json::getString (parsed.value, "download_url").value_or ("") : juce::String();
            // Only offer https links (or local http while developing). Context never downloads updates itself.
            if (url.startsWithIgnoreCase ("https://") || (url.startsWithIgnoreCase ("http://") && urls::parseBaseUrl (url.upToLastOccurrenceOf ("/", false, false)).kind == urls::HostKind::Loopback))
                info.downloadUrl = url.substring (0, 512);
            info.available = info.latestVersion.isNotEmpty() && isNewerVersion (info.latestVersion, cfg.pluginVersion);
        }
        {
            const juce::ScopedLock sl (lock);
            update = info;
        }
        sendChangeMessage();
    });
}
} // namespace context
