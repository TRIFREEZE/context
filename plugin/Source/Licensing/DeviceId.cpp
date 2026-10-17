#include "DeviceId.h"
#include "Core/AppPaths.h"
#include <juce_cryptography/juce_cryptography.h>

namespace context
{
juce::String DeviceId::hashIdentifier (const juce::String& raw)
{
    const auto text = "context-device-v1:" + raw.trim().toLowerCase();
    return juce::SHA256 (text.toUTF8()).toHexString().substring (0, 32);
}

juce::String DeviceId::get()
{
    static juce::String cached;
    static juce::CriticalSection lock;
    const juce::ScopedLock sl (lock);
    if (cached.isNotEmpty())
        return cached;

    juce::String raw;
   #if JUCE_WINDOWS
    raw = juce::WindowsRegistry::getValue ("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Cryptography\\MachineGuid", {},
                                           juce::WindowsRegistry::WoW64_64bit);
   #endif
    if (raw.trim().isEmpty())
        raw = juce::SystemStats::getUniqueDeviceID();

    if (raw.trim().isEmpty())
    {
        // Last resort: a random identifier persisted locally.
        auto f = AppPaths::root().getChildFile ("device.id");
        raw = f.existsAsFile() ? f.loadFileAsString().trim() : juce::String();
        if (raw.length() < 16)
        {
            raw = juce::Uuid().toString();
            f.replaceWithText (raw);
        }
    }

    cached = hashIdentifier (raw);
    return cached;
}
} // namespace context
