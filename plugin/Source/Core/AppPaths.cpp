#include "AppPaths.h"
#include "BuildInfo.h"

namespace context
{
namespace
{
    juce::CriticalSection& overrideLock()
    {
        static juce::CriticalSection lock;
        return lock;
    }

    juce::File& overrideDir()
    {
        static juce::File dir;
        return dir;
    }
}

void AppPaths::setRootOverride (const juce::File& dir)
{
    const juce::ScopedLock sl (overrideLock());
    overrideDir() = dir;
}

juce::File AppPaths::root()
{
    juce::File dir;
    {
        const juce::ScopedLock sl (overrideLock());
        dir = overrideDir();
    }

    if (dir == juce::File())
    {
        const auto env = juce::SystemStats::getEnvironmentVariable ("CONTEXT_APPDATA_DIR", {});
        if (env.isNotEmpty() && juce::File::isAbsolutePath (env))
            dir = juce::File (env);
    }

    if (dir == juce::File())
    {
       #if JUCE_MAC
        dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                  .getChildFile ("Application Support").getChildFile (build::productName);
       #else
        dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                  .getChildFile (build::productName);
       #endif
    }

    if (! dir.isDirectory())
        dir.createDirectory();   // failure is tolerated; callers handle unwritable folders

    return dir;
}
} // namespace context
