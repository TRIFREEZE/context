#pragma once

#include <juce_core/juce_core.h>

namespace context
{
/** Privacy-conscious, stable device identifier sent to the licensing server.

    deviceId = first 32 hex chars of SHA-256("context-device-v1:" + machineIdentifier)

    machineIdentifier:
      Windows: HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid (random GUID created at OS install)
      other:   juce::SystemStats::getUniqueDeviceID()
      fallback: a random ID generated once and stored in the app-data folder

    No MAC addresses, serial numbers, user names or hardware lists are collected,
    and the raw identifier never leaves the computer — only the hash does. */
struct DeviceId
{
    static juce::String get();
    static juce::String hashIdentifier (const juce::String& raw);   // exposed for tests
};
} // namespace context
