#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "Core/AppPaths.h"
#include <functional>

namespace context::test
{
/** Every test run gets its own empty app-data folder so tests never touch real user data. */
inline juce::File& isolatedRoot()
{
    static juce::File dir;
    return dir;
}

inline void ensureIsolatedAppDataDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("ContextTests-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
    dir.createDirectory();
    isolatedRoot() = dir;
    AppPaths::setRootOverride (dir);
}

inline juce::String& integrationBridgeUrl()
{
    static juce::String url;
    return url;
}

/** Spin the message loop until pred() is true or the timeout expires. */
inline bool waitUntil (std::function<bool()> pred, int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (juce::Time::getMillisecondCounter() < end)
    {
        if (pred())
            return true;
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    }
    return pred();
}

/** A fresh scratch folder under the isolated root. */
inline juce::File scratchDir (const juce::String& name)
{
    auto d = isolatedRoot().getChildFile ("scratch").getChildFile (name + "-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    d.createDirectory();
    return d;
}
} // namespace context::test
