#include "LicenseToken.h"
#include "Utils/Json.h"
#include "monocypher-ed25519.h"

namespace context
{
bool LicenseToken::base64UrlDecode (const juce::String& in, juce::MemoryBlock& out)
{
    if (in.isEmpty() || in.length() > 8192 || ! in.containsOnly ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_="))
        return false;
    auto s = in.replaceCharacter ('-', '+').replaceCharacter ('_', '/').trimCharactersAtEnd ("=");
    while (s.length() % 4 != 0)
        s << "=";
    out.reset();
    juce::MemoryOutputStream mo (out, false);
    return juce::Base64::convertFromBase64 (mo, s);
}

std::optional<juce::Time> LicenseToken::parseIsoUtc (const juce::String& s)
{
    // Strict "YYYY-MM-DDTHH:MM:SSZ"
    if (s.length() != 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s[19] != 'Z')
        return std::nullopt;
    const int y = s.substring (0, 4).getIntValue(), mo = s.substring (5, 7).getIntValue(), d = s.substring (8, 10).getIntValue();
    const int h = s.substring (11, 13).getIntValue(), mi = s.substring (14, 16).getIntValue(), se = s.substring (17, 19).getIntValue();
    if (y < 2000 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60)
        return std::nullopt;
    return juce::Time (y, mo - 1, d, h, mi, se, 0, false);   // false = UTC
}

juce::String LicenseToken::checkToString (Check c)
{
    switch (c)
    {
        case Check::Valid:           return "valid";
        case Check::BadFormat:       return "malformed token";
        case Check::BadSignature:    return "signature check failed";
        case Check::WrongProduct:    return "token is for another product";
        case Check::WrongDevice:     return "token belongs to another device";
        case Check::Expired:         return "offline grace period ended";
        case Check::ClockSuspicious: return "system clock appears to be wrong";
        case Check::NoPublicKey:     return "this build has no license public key";
    }
    return "unknown";
}

std::pair<LicenseToken::Check, std::optional<LicenseToken>>
LicenseToken::parseAndVerify (const juce::String& token, const juce::String& publicKeyBase64, const juce::String& expectedDeviceId)
{
    juce::MemoryBlock pub;
    {
        juce::MemoryOutputStream mo (pub, false);
        if (publicKeyBase64.isEmpty() || ! juce::Base64::convertFromBase64 (mo, publicKeyBase64))
            return { Check::NoPublicKey, std::nullopt };
    }
    if (pub.getSize() != 32)
        return { Check::NoPublicKey, std::nullopt };

    const int dot = token.indexOfChar ('.');
    if (dot <= 0 || dot != token.lastIndexOfChar ('.'))
        return { Check::BadFormat, std::nullopt };

    const auto body = token.substring (0, dot);
    juce::MemoryBlock payloadBytes, sig;
    if (! base64UrlDecode (body, payloadBytes) || ! base64UrlDecode (token.substring (dot + 1), sig) || sig.getSize() != 64)
        return { Check::BadFormat, std::nullopt };

    const auto bodyUtf8 = body.toStdString();
    if (crypto_ed25519_check (static_cast<const uint8_t*> (sig.getData()), static_cast<const uint8_t*> (pub.getData()),
                              reinterpret_cast<const uint8_t*> (bodyUtf8.data()), bodyUtf8.size()) != 0)
        return { Check::BadSignature, std::nullopt };

    const auto parsed = json::parseUntrusted (payloadBytes.toString(), 16 * 1024, 8);
    if (! parsed.ok || ! json::isObject (parsed.value))
        return { Check::BadFormat, std::nullopt };
    const auto& v = parsed.value;

    if (json::getString (v, "product").value_or ("") != "context" || json::getNumber (v, "v").value_or (0) != 1.0)
        return { Check::WrongProduct, std::nullopt };

    LicenseToken t;
    t.raw = token;
    t.deviceId = json::getString (v, "device_id").value_or ("");
    t.licenseHint = json::getString (v, "license_hint").value_or ("").substring (0, 8);
    t.maxDevices = (int) json::getNumber (v, "max_devices").value_or (0);
    const auto iat = parseIsoUtc (json::getString (v, "iat").value_or (""));
    const auto refresh = parseIsoUtc (json::getString (v, "refresh_after").value_or (""));
    const auto offline = parseIsoUtc (json::getString (v, "offline_until").value_or (""));
    if (! iat || ! refresh || ! offline)
        return { Check::BadFormat, std::nullopt };
    t.issuedAt = *iat;
    t.refreshAfter = *refresh;
    t.offlineUntil = *offline;
    if (auto e = json::getString (v, "license_expires_at"))
        t.licenseExpiresAt = parseIsoUtc (*e);

    if (t.deviceId != expectedDeviceId)
        return { Check::WrongDevice, std::nullopt };

    return { Check::Valid, t };
}

LicenseToken::Check LicenseToken::checkTime (juce::Time now) const
{
    // A clock more than 2 days before the issue time means the system clock was
    // rolled back (or is badly wrong); don't extend the grace window in that case.
    if (now < issuedAt - juce::RelativeTime::days (2))
        return Check::ClockSuspicious;
    if (now >= offlineUntil)
        return Check::Expired;
    return Check::Valid;
}
} // namespace context
