#include "TestSupport.h"
#include "Utils/FilenameSanitizer.h"
#include "Utils/Redaction.h"
#include "Utils/UrlSafety.h"
#include "Utils/Json.h"
#include "Utils/AtomicFile.h"
#include "Core/Settings.h"
#include "Core/SerialWorker.h"

namespace context::test
{
class FilenameTests final : public juce::UnitTest
{
public:
    FilenameTests() : juce::UnitTest ("Filename sanitizer", "Utils") {}

    void runTest() override
    {
        using namespace filenames;
        beginTest ("Invalid Windows characters removed");
        {
            const auto s = sanitizeComponent ("a<b>c:d\"e/f\\g|h?i*j");
            for (auto c : juce::String ("<>:\"/\\|?*"))
                expect (! s.containsChar (c), "contains " + juce::String::charToString (c) + ": " + s);
            expectEquals (s, juce::String ("a_b_c_d_e_f_g_h_i_j"));
        }

        beginTest ("Control characters, traversal and trailing dots");
        expectEquals (sanitizeComponent ("..\\..\\windows\\system32"), juce::String ("windows_system32"));
        expectEquals (sanitizeComponent ("name.   "), juce::String ("name"));
        expectEquals (sanitizeComponent (juce::String ("x") + juce::String::charToString (7) + "y"), juce::String ("x_y"));
        expectEquals (sanitizeComponent (""), juce::String ("untitled"));
        expectEquals (sanitizeComponent ("....."), juce::String ("untitled"));

        beginTest ("Reserved device names");
        expectEquals (sanitizeComponent ("CON"), juce::String ("_CON"));
        expectEquals (sanitizeComponent ("nul.wav"), juce::String ("_nul.wav"));
        expectEquals (sanitizeComponent ("COM1"), juce::String ("_COM1"));

        beginTest ("Length limited and unicode kept");
        expect (sanitizeComponent (juce::String::repeatedString ("a", 500), 80).length() <= 80);
        const auto uni = sanitizeComponent (juce::CharPointer_UTF8 ("caf\xc3\xa9 \xe2\x9c\xa8 vocal"));
        expect (uni.contains (juce::CharPointer_UTF8 ("caf\xc3\xa9")));

        beginTest ("Generated name");
        const auto when = juce::Time (2026, 9, 16, 12, 0);   // month is 0-based: October
        expectEquals (generatedBaseName ("Context", 142.0, "F#", "Minor", "Vocal", when),
                      juce::String ("Context_142BPM_FSharpMinor_Vocal_2026-10-16"));
        expectEquals (generatedBaseName ("Context", std::nullopt, "", "", "One Shot", when),
                      juce::String ("Context_OneShot_2026-10-16"));
        expectEquals (keyForFilename ("Bb"), juce::String ("BFlat"));

        beginTest ("Unique files never collide");
        auto dir = scratchDir ("unique");
        auto f1 = uniqueFile (dir, "Context_X", ".wav");
        expectEquals (f1.getFileName(), juce::String ("Context_X_001.wav"));
        f1.create();
        auto f2 = uniqueFile (dir, "Context_X", ".wav");
        expectEquals (f2.getFileName(), juce::String ("Context_X_002.wav"));

        beginTest ("isInsideDirectory");
        expect (isInsideDirectory (dir.getChildFile ("a/b.wav"), dir));
        expect (! isInsideDirectory (dir.getChildFile ("../outside.wav"), dir));
        expect (! isInsideDirectory (dir, dir));
        expect (! isInsideDirectory (dir.getSiblingFile (dir.getFileName() + "-evil").getChildFile ("x.wav"), dir));
    }
};

class RedactionTests final : public juce::UnitTest
{
public:
    RedactionTests() : juce::UnitTest ("Secret redaction", "Utils") {}

    void runTest() override
    {
        using namespace redaction;
        beginTest ("License keys masked");
        const auto r1 = redact ("activating CTX-AB12-CD34-EF56-GH78 now");
        expect (! r1.contains ("AB12") && ! r1.contains ("EF56"), r1);
        expect (r1.contains ("GH78"), r1);
        expectEquals (maskLicenseKey ("CTX-AB12-CD34-EF56-GH78"), juce::String ("CTX-****-****-****-GH78"));

        beginTest ("Headers, bearer, cookies, json secrets, jwt, url credentials");
        const auto r2 = redact ("Authorization: Bearer abc.def.ghi\nCookie: __session=xyz; other=1");
        expect (! r2.contains ("abc.def") && ! r2.contains ("xyz"), r2);
        const auto r3 = redact (R"({"session_token":"s3cr3t","prompt":"kick","password":"hunter2"})");
        expect (! r3.contains ("s3cr3t") && ! r3.contains ("hunter2") && r3.contains ("kick"), r3);
        const auto r4 = redact ("GET /x?api_key=SECRET1&q=kick");
        expect (! r4.contains ("SECRET1") && r4.contains ("q=kick"), r4);
        const auto r5 = redact ("token eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.dozjgNryP4J3jVmNHl0w5N_XgL0n3I9PlFUP0THsR8U end");
        expect (! r5.contains ("eyJhbGciOiJIUzI1NiJ9"), r5);
        const auto r6 = redact ("http://user:pa55@127.0.0.1:3000/x");
        expect (! r6.contains ("pa55") && ! r6.contains ("user:"), r6);

        beginTest ("Ordinary text untouched; huge input bounded");
        expectEquals (redact ("dark distorted female vocal chop"), juce::String ("dark distorted female vocal chop"));
        const auto huge = redact (juce::String::repeatedString ("a", 200000));
        expect (huge.length() < 9000);

        beginTest ("safeUrlForDisplay");
        expectEquals (safeUrlForDisplay ("http://u:p@127.0.0.1:3000/a?token=1"), juce::String ("http://127.0.0.1:3000/a"));
    }
};

class UrlSafetyTests final : public juce::UnitTest
{
public:
    UrlSafetyTests() : juce::UnitTest ("URL safety", "Utils") {}

    void runTest() override
    {
        using namespace urls;
        beginTest ("Loopback addresses");
        for (auto u : { "http://127.0.0.1:3000", "http://localhost:3000/", "http://[::1]:3000", "http://127.0.0.2:8080" })
        {
            const auto p = parseBaseUrl (u);
            expect (p.ok, u + juce::String (" ") + p.error);
            expect (p.kind == HostKind::Loopback, u);
        }
        expectEquals (parseBaseUrl ("http://localhost:3000/").normalized, juce::String ("http://localhost:3000"));
        expectEquals (parseBaseUrl ("HTTP://LocalHost:3000").normalized, juce::String ("http://localhost:3000"));

        beginTest ("LAN and public addresses classified");
        expect (parseBaseUrl ("http://192.168.1.20:3000").kind == HostKind::PrivateNetwork);
        expect (parseBaseUrl ("http://10.0.0.5:3000").kind == HostKind::PrivateNetwork);
        expect (parseBaseUrl ("http://172.20.1.1").kind == HostKind::PrivateNetwork);
        expect (parseBaseUrl ("http://studio-pc:3000").kind == HostKind::PrivateNetwork);
        expect (parseBaseUrl ("https://bridge.example.com").kind == HostKind::Public);
        expect (parseBaseUrl ("http://8.8.8.8").kind == HostKind::Public);

        beginTest ("Rejected inputs");
        for (auto u : { "", "127.0.0.1:3000", "ftp://127.0.0.1", "file:///c:/x", "http://user:pw@127.0.0.1:3000",
                        "http://127.0.0.1:3000/api", "http://127.0.0.1:99999", "http://127.0.0.1:3000?x=1",
                        "javascript:alert(1)", "http://", "http://bad host" })
            expect (! parseBaseUrl (u).ok, juce::String ("should reject: ") + u);

        beginTest ("Path join escapes job IDs");
        expectEquals (joinPath ("http://127.0.0.1:3000", "/context/v1/jobs/{id}", "a/../b c"),
                      juce::String ("http://127.0.0.1:3000/context/v1/jobs/a%2F..%2Fb%20c"));

        beginTest ("Download URL policy");
        const auto base = parseBaseUrl ("http://127.0.0.1:3000");
        expect (checkDownloadUrl ("/files/a.wav", base).ok);
        expectEquals (checkDownloadUrl ("/files/a.wav", base).url, juce::String ("http://127.0.0.1:3000/files/a.wav"));
        expect (checkDownloadUrl ("https://cdn.example.com/a.wav", base).ok);
        expect (checkDownloadUrl ("http://localhost:9000/a.wav", base).ok);
        expect (! checkDownloadUrl ("http://evil.example.com/a.wav", base).ok);
        expect (! checkDownloadUrl ("file:///etc/passwd", base).ok);
        expect (! checkDownloadUrl ("/files/../../secret", base).ok);
        expect (! checkDownloadUrl ("https://u:p@cdn.example.com/a.wav", base).ok);
        expect (! checkDownloadUrl ("//evil.example.com/a.wav", base).ok);
    }
};

class JsonSafetyTests final : public juce::UnitTest
{
public:
    JsonSafetyTests() : juce::UnitTest ("JSON safety", "Utils") {}

    void runTest() override
    {
        beginTest ("Malformed / empty / oversized / deeply nested rejected");
        expect (! json::parseUntrusted ("{not json").ok);
        expect (! json::parseUntrusted ("").ok);
        expect (! json::parseUntrusted (juce::String::repeatedString ("[", 100000)).ok);
        expect (! json::parseUntrusted ("\"" + juce::String::repeatedString ("x", 3 * 1024 * 1024) + "\"").ok);
        expect (json::parseUntrusted (R"({"a":"[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[["})").ok,
                "brackets inside strings must not count as nesting");

        beginTest ("Path lookup");
        const auto v = json::parseUntrusted (R"({"data":[{"id":"abc","n":3,"s":"4.5"}],"ok":true})").value;
        expectEquals (*json::getString (v, "data.0.id"), juce::String ("abc"));
        expectEquals (*json::getNumber (v, "data.0.n"), 3.0);
        expectEquals (*json::getNumber (v, "data.0.s"), 4.5);
        expect (*json::getBool (v, "ok"));
        expect (! json::getString (v, "data.5.id").has_value());
        expect (! json::getString (v, "data.0.id.x").has_value());
        expect (! json::getString (juce::var(), "a").has_value());

        beginTest ("Field path validation");
        expect (json::isValidFieldPath ("data.0.audio_url"));
        expect (! json::isValidFieldPath ("data..x"));
        expect (! json::isValidFieldPath ("a/b"));
        expect (! json::isValidFieldPath ("$(rm -rf)"));
    }
};

class SettingsTests final : public juce::UnitTest
{
public:
    SettingsTests() : juce::UnitTest ("Settings store", "Storage") {}

    void runTest() override
    {
        beginTest ("Values clamped");
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("captureSeconds", 9999);
        o->setProperty ("cacheLimitMB", -5);
        o->setProperty ("providerId", "evil");
        o->setProperty ("logLevel", "verbose!!");
        o->setProperty ("previewGainDb", 40.0);
        const auto s = Settings::fromVar (juce::var (o.get()));
        expectEquals (s.captureSeconds, 30);
        expectEquals (s.cacheLimitMB, 100);
        expectEquals (s.providerId, juce::String ("bridge"));
        expectEquals (s.logLevel, juce::String ("info"));
        expectEquals (s.previewGainDb, 0.0f);

        beginTest ("Corrupt settings file recovers to defaults");
        auto dir = scratchDir ("settings");
        auto file = dir.getChildFile ("settings.json");
        file.replaceWithText ("{\"captureSeconds\": 12, broken");
        SerialWorker worker ("test-file-worker");
        {
            SettingsStore store (file, worker);
            expect (store.wasRecoveredFromCorruption());
            expectEquals (store.get().captureSeconds, Settings::defaults().captureSeconds);
            expect (dir.findChildFiles (juce::File::findFiles, false, "settings.corrupt-*").size() == 1);

            store.update ([] (Settings& st) { st.captureSeconds = 12; });
            expect (worker.waitUntilIdle (5000));
        }
        SettingsStore reloaded (file, worker);
        expect (! reloaded.wasRecoveredFromCorruption());
        expectEquals (reloaded.get().captureSeconds, 12);

        beginTest ("Atomic write replaces content");
        auto f = dir.getChildFile ("atomic.txt");
        expect (atomicfile::writeText (f, "one"));
        expect (atomicfile::writeText (f, "two"));
        expectEquals (f.loadFileAsString(), juce::String ("two"));
        expectEquals (dir.findChildFiles (juce::File::findFiles, false, "*.tmp").size(), 0);
    }
};

static FilenameTests filenameTests;
static RedactionTests redactionTests;
static UrlSafetyTests urlSafetyTests;
static JsonSafetyTests jsonSafetyTests;
static SettingsTests settingsTests;
} // namespace context::test
