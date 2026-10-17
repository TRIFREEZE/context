#include "TestSupport.h"
#include "Licensing/LicenseManager.h"
#include "Licensing/DeviceId.h"
#include "Utils/Json.h"
#include "monocypher-ed25519.h"

namespace context::test
{
namespace
{
    juce::String b64url (const void* data, size_t size)
    {
        return juce::Base64::toBase64 (data, size).replaceCharacter ('+', '-').replaceCharacter ('/', '_').trimCharactersAtEnd ("=");
    }

    /** UTC "YYYY-MM-DDTHH:MM:SSZ" (juce::Time::formatted uses local time, so shift by the UTC offset). */
    juce::String iso (juce::Time t)
    {
        return juce::Time (t.toMilliseconds() - (juce::int64) t.getUTCOffsetSeconds() * 1000).formatted ("%Y-%m-%dT%H:%M:%SZ");
    }

    /** Test-only signer (the real private key lives only on the licensing server). */
    struct TestSigner
    {
        uint8_t secret[64], pub[32];
        TestSigner (uint8_t seedByte = 7)
        {
            uint8_t seed[32];
            std::fill (std::begin (seed), std::end (seed), seedByte);
            crypto_ed25519_key_pair (secret, pub, seed);
        }
        juce::String publicKey() const { return juce::Base64::toBase64 (pub, 32); }

        juce::String sign (const juce::String& device, juce::Time iat, juce::RelativeTime refresh = juce::RelativeTime::hours (24),
                           juce::RelativeTime offline = juce::RelativeTime::days (7), const juce::String& product = "context") const
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("v", 1);
            o->setProperty ("product", product);
            o->setProperty ("license_hint", "ABCD");
            o->setProperty ("device_id", device);
            o->setProperty ("iat", iso (iat));
            o->setProperty ("refresh_after", iso (iat + refresh));
            o->setProperty ("offline_until", iso (iat + offline));
            o->setProperty ("max_devices", 2);
            const auto payload = json::toCompactString (juce::var (o)).toStdString();
            const auto body = b64url (payload.data(), payload.size());
            const auto bodyStd = body.toStdString();
            uint8_t sig[64];
            crypto_ed25519_sign (sig, secret, reinterpret_cast<const uint8_t*> (bodyStd.data()), bodyStd.size());
            return body + "." + b64url (sig, 64);
        }
    };

    juce::Time fixedNow() { return juce::Time (2026, 9, 16, 12, 0, 0, 0, false); }
}

class LicenseTokenTests final : public juce::UnitTest
{
public:
    LicenseTokenTests() : juce::UnitTest ("License tokens", "Licensing") {}

    void runTest() override
    {
        TestSigner signer;
        const juce::String device = "0123456789abcdef0123456789abcdef";
        const auto now = fixedNow();

        beginTest ("Valid token verifies; time windows");
        auto token = signer.sign (device, now);
        auto [check, t] = LicenseToken::parseAndVerify (token, signer.publicKey(), device);
        expect (check == LicenseToken::Check::Valid, LicenseToken::checkToString (check));
        expect (t.has_value());
        if (t)
        {
            expect (t->checkTime (now) == LicenseToken::Check::Valid);
            expect (! t->needsRefresh (now + juce::RelativeTime::hours (23)));
            expect (t->needsRefresh (now + juce::RelativeTime::hours (25)));
            expect (t->checkTime (now + juce::RelativeTime::days (6)) == LicenseToken::Check::Valid);
            expect (t->checkTime (now + juce::RelativeTime::days (7) + juce::RelativeTime::seconds (1)) == LicenseToken::Check::Expired);
            expect (t->checkTime (now - juce::RelativeTime::days (5)) == LicenseToken::Check::ClockSuspicious);
            expectEquals (t->licenseHint, juce::String ("ABCD"));
        }

        beginTest ("Tampering, other keys, other devices, junk");
        expect (LicenseToken::parseAndVerify (token, signer.publicKey(), "ffffffffffffffffffffffffffffffff").first == LicenseToken::Check::WrongDevice);
        TestSigner other (9);
        expect (LicenseToken::parseAndVerify (other.sign (device, now), signer.publicKey(), device).first == LicenseToken::Check::BadSignature);
        auto tampered = token;
        tampered = tampered.replaceSection (5, 1, tampered[5] == 'A' ? "B" : "A");
        expect (LicenseToken::parseAndVerify (tampered, signer.publicKey(), device).first != LicenseToken::Check::Valid);
        expect (LicenseToken::parseAndVerify (signer.sign (device, now, juce::RelativeTime::hours (24), juce::RelativeTime::days (7), "other"),
                                              signer.publicKey(), device).first == LicenseToken::Check::WrongProduct);
        for (auto junk : { "", ".", "a.b", "a.b.c", "not-a-token", "!!!!.@@@@" })
            expect (LicenseToken::parseAndVerify (junk, signer.publicKey(), device).first != LicenseToken::Check::Valid, junk);
        expect (LicenseToken::parseAndVerify (token, "", device).first == LicenseToken::Check::NoPublicKey);
        expect (LicenseToken::parseAndVerify (token, "c2hvcnQ=", device).first == LicenseToken::Check::NoPublicKey);

        beginTest ("ISO parser is strict");
        expect (LicenseToken::parseIsoUtc ("2026-10-16T12:00:00Z").has_value());
        expect (! LicenseToken::parseIsoUtc ("2026-13-16T12:00:00Z").has_value());
        expect (! LicenseToken::parseIsoUtc ("2026-10-16 12:00:00").has_value());

        beginTest ("Device ID is a 32-hex hash and stable");
        const auto id = DeviceId::get();
        expectEquals (id.length(), 32);
        expect (id.containsOnly ("0123456789abcdef"));
        expectEquals (DeviceId::get(), id);
        expect (DeviceId::hashIdentifier ("ABC") == DeviceId::hashIdentifier ("abc "));
        expect (DeviceId::hashIdentifier ("abc") != DeviceId::hashIdentifier ("abd"));

        beginTest ("Version comparison");
        expect (LicenseManager::isNewerVersion ("1.1.0", "1.0.9"));
        expect (LicenseManager::isNewerVersion ("v0.2", "0.1.5"));
        expect (! LicenseManager::isNewerVersion ("0.1.0", "0.1.0"));
        expect (! LicenseManager::isNewerVersion ("0.0.9", "0.1.0"));
    }
};

class LicenseManagerOfflineTests final : public juce::UnitTest
{
public:
    LicenseManagerOfflineTests() : juce::UnitTest ("License manager (offline grace)", "Licensing") {}

    void runTest() override
    {
        TestSigner signer;
        const auto device = DeviceId::get();
        auto dir = scratchDir ("license");
        auto stateFile = dir.getChildFile ("license.json");
        juce::Time now = fixedNow();

        auto make = [&]
        {
            LicenseManager::Config c;
            c.stateFile = stateFile;
            c.publicKeyBase64 = signer.publicKey();
            c.serverUrl = [] { return juce::String ("http://127.0.0.1:1"); };   // nothing listens: server "offline"
            c.pluginVersion = "0.1.0";
            c.deviceId = device;
            c.clock = [&now] { return now; };
            return std::make_unique<LicenseManager> (c);
        };

        beginTest ("No state file: unlicensed, generation disabled");
        {
            auto lm = make();
            expect (! lm->isLicensed());
            expect (lm->getStatus() == LicenseManager::Status::Unlicensed);
        }

        beginTest ("Stored token: licensed; server offline keeps grace; expiry after 7 days");
        {
            stateFile.replaceWithText (R"({"licenseKey":"CTX-AAAA-BBBB-CCCC-ABCD","token":")" + signer.sign (device, now) + "\"}");
            auto lm = make();
            expect (lm->isLicensed());
            expect (lm->getStatus() == LicenseManager::Status::Licensed);
            expectEquals (lm->getMaskedKey(), juce::String ("CTX-****-****-****-ABCD"));

            now = fixedNow() + juce::RelativeTime::days (3);
            lm->refreshIfDue();
            expect (lm->waitForIdle (20000));
            expect (lm->isLicensed(), "offline grace must keep the license");
            expect (lm->getStatus() == LicenseManager::Status::Grace);

            now = fixedNow() + juce::RelativeTime::days (8);
            lm->refreshIfDue();
            expect (lm->waitForIdle (20000));
            expect (! lm->isLicensed());
            expect (lm->getStatus() == LicenseManager::Status::Expired);
            expect (lm->getMessage().contains ("passthrough is unaffected"));
            now = fixedNow();
        }

        beginTest ("Corrupt / foreign / tampered state files never crash");
        for (auto content : { juce::String ("{garbage"), juce::String (R"({"token":"x.y"})"),
                              juce::String (R"({"licenseKey":"CTX-AAAA-BBBB-CCCC-ABCD","token":")") + TestSigner (3).sign (device, now) + "\"}",
                              juce::String (R"({"licenseKey":"CTX-AAAA-BBBB-CCCC-ABCD","token":")") + signer.sign ("ffffffffffffffffffffffffffffffff", now) + "\"}",
                              juce::String::repeatedString ("[", 70000) })
        {
            stateFile.replaceWithText (content);
            auto lm = make();
            expect (! lm->isLicensed());
        }

        beginTest ("Bad key format rejected locally; unreachable server reported");
        {
            stateFile.deleteFile();
            auto lm = make();
            lm->activate ("hello");
            expect (lm->getMessage().contains ("CTX-XXXX"));
            lm->activate ("CTX-AAAA-BBBB-CCCC-DDDD");
            expect (lm->waitForIdle (20000));
            expect (waitUntil ([&] { return lm->getStatus() != LicenseManager::Status::Activating; }, 5000));
            expect (lm->getMessage().contains ("Couldn't reach"), lm->getMessage());
            expect (! lm->isLicensed());
        }

        beginTest ("License keys are never sent over plain http to another host");
        {
            stateFile.deleteFile();
            LicenseManager::Config c;
            c.stateFile = stateFile;
            c.publicKeyBase64 = signer.publicKey();
            c.serverUrl = [] { return juce::String ("http://licensing.example.com"); };
            c.deviceId = device;
            LicenseManager lm (c);
            lm.activate ("CTX-AAAA-BBBB-CCCC-DDDD");
            expect (lm.getMessage().contains ("https://"), lm.getMessage());
            expect (lm.waitForIdle (1000));
        }

        beginTest ("Build without a public key can't be activated but loads fine");
        {
            LicenseManager::Config c;
            c.stateFile = stateFile;
            c.deviceId = device;
            c.serverUrl = [] { return juce::String ("http://127.0.0.1:1"); };
            LicenseManager lm (c);
            expect (lm.getStatus() == LicenseManager::Status::NotConfigured);
            expect (! lm.isLicensed());
        }
    }
};

class LicenseIntegrationTests final : public juce::UnitTest
{
public:
    LicenseIntegrationTests() : juce::UnitTest ("Licensing backend end-to-end", "LicenseIntegration") {}

    std::unique_ptr<LicenseManager> make (const juce::File& state, const juce::String& deviceId)
    {
        LicenseManager::Config c;
        c.stateFile = state;
        c.publicKeyBase64 = integrationUrls()["license-public-key"];
        const auto server = integrationUrls()["license-server"];
        c.serverUrl = [server] { return server; };
        c.pluginVersion = "0.1.0";
        c.deviceId = deviceId;
        return std::make_unique<LicenseManager> (c);
    }

    bool settle (LicenseManager& lm)
    {
        return lm.waitForIdle (20000) && waitUntil ([&] { return lm.getStatus() != LicenseManager::Status::Activating; }, 5000);
    }

    void runTest() override
    {
        beginTest ("Server and key supplied");
        expect (integrationUrls()["license-server"].startsWith ("http://"));
        if (integrationUrls()["license-server"].isEmpty())
            return;

        auto dir = scratchDir ("license-e2e");
        const juce::String devA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", devB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

        beginTest ("Activate against the real backend; token verifies with the compiled-style public key");
        {
            auto lm = make (dir.getChildFile ("a.json"), devA);
            lm->activate (integrationUrls()["license-key"]);
            expect (settle (*lm));
            expect (lm->isLicensed(), LicenseManager::toString (lm->getStatus()) + ": " + lm->getMessage());
            expect (dir.getChildFile ("a.json").existsAsFile());

            // Reload from disk (DAW restart) -> still licensed without network.
            auto again = make (dir.getChildFile ("a.json"), devA);
            expect (again->isLicensed());
            again->refreshIfDue (true);
            expect (settle (*again));
            expect (again->isLicensed());
            expect (again->getStatus() == LicenseManager::Status::Licensed);
        }

        beginTest ("Device limit message comes from the server");
        {
            auto first = make (dir.getChildFile ("s1.json"), devA);
            first->activate (integrationUrls()["license-key-single"]);
            expect (settle (*first) && first->isLicensed());
            auto second = make (dir.getChildFile ("s2.json"), devB);
            second->activate (integrationUrls()["license-key-single"]);
            expect (settle (*second));
            expect (! second->isLicensed());
            expect (second->getMessage().contains ("already active"), second->getMessage());

            first->deactivate();
            expect (first->waitForIdle (20000));
            second->activate (integrationUrls()["license-key-single"]);
            expect (settle (*second) && second->isLicensed(), "slot freed by deactivate");
        }

        beginTest ("Revocation is enforced at the next validation");
        {
            auto lm = make (dir.getChildFile ("r.json"), devA);
            lm->activate (integrationUrls()["license-key-revoke"]);
            expect (settle (*lm) && lm->isLicensed());

            juce::ChildProcess py;
            const auto script = "import os,sys; os.environ['CONTEXT_LICENSE_DB']=r'" + integrationUrls()["license-db"]
                              + "'; sys.path.insert(0, r'" + integrationUrls()["backend-dir"] + "'); from app import admin; "
                              + "sys.exit(admin.main(['revoke', '" + integrationUrls()["license-key-revoke"] + "']))";
            expect (py.start (juce::StringArray { integrationUrls()["python"], "-c", script }));
            py.waitForProcessToFinish (30000);
            expectEquals ((int) py.getExitCode(), 0, py.readAllProcessOutput());

            lm->refreshIfDue (true);
            expect (settle (*lm));
            expect (! lm->isLicensed());
            expect (lm->getStatus() == LicenseManager::Status::Invalid);
            expect (lm->getMessage().contains ("revoked"), lm->getMessage());
        }

        beginTest ("Wrong public key in the plugin is detected");
        {
            LicenseManager::Config c;
            c.stateFile = dir.getChildFile ("w.json");
            c.publicKeyBase64 = TestSigner (11).publicKey();
            const auto server = integrationUrls()["license-server"];
            c.serverUrl = [server] { return server; };
            c.pluginVersion = "0.1.0";
            c.deviceId = devB;
            LicenseManager lm (c);
            lm.activate (integrationUrls()["license-key"]);
            expect (settle (lm));
            expect (! lm.isLicensed());
            expect (lm.getMessage().contains ("different signing keys"), lm.getMessage());
        }

        beginTest ("Update check");
        {
            auto lm = make (dir.getChildFile ("u.json"), devA);
            lm->checkForUpdate();
            expect (lm->waitForIdle (20000));
            const auto u = lm->getUpdateInfo();
            expect (u.checked && u.available);
            expectEquals (u.latestVersion, juce::String ("9.9.9"));
            expectEquals (u.downloadUrl, juce::String ("https://example.com/context"));
        }
    }
};

static LicenseTokenTests licenseTokenTests;
static LicenseManagerOfflineTests licenseManagerOfflineTests;
static LicenseIntegrationTests licenseIntegrationTests;
} // namespace context::test
