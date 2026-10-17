#pragma once

#include <juce_core/juce_core.h>
#include <optional>

namespace context
{
/** A license token issued by the Context licensing server:
        base64url(payload_json) "." base64url(Ed25519 signature over the first part)
    Verified offline with the PUBLIC key compiled into the plugin. */
struct LicenseToken
{
    juce::String raw;
    juce::String deviceId;
    juce::String licenseHint;            // last 4 chars of the key
    juce::Time issuedAt, refreshAfter, offlineUntil;
    std::optional<juce::Time> licenseExpiresAt;
    int maxDevices = 0;

    enum class Check { Valid, BadFormat, BadSignature, WrongProduct, WrongDevice, Expired, ClockSuspicious, NoPublicKey };

    /** Parses and verifies signature, product and device. Does not check time. */
    static std::pair<Check, std::optional<LicenseToken>> parseAndVerify (const juce::String& token,
                                                                          const juce::String& publicKeyBase64,
                                                                          const juce::String& expectedDeviceId);

    /** Time checks: still inside the offline window and the clock not obviously rolled back. */
    Check checkTime (juce::Time now) const;
    bool needsRefresh (juce::Time now) const { return now >= refreshAfter; }

    static juce::String checkToString (Check);
    static std::optional<juce::Time> parseIsoUtc (const juce::String&);
    static bool base64UrlDecode (const juce::String& in, juce::MemoryBlock& out);
};
} // namespace context
