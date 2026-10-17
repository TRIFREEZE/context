#pragma once

#include <juce_core/juce_core.h>

namespace context
{
/** Where Context keeps its local files.
    Windows: %APPDATA%\<Product>   macOS: ~/Library/Application Support/<Product>   Linux: ~/.config/<Product>

    The root can be overridden with the CONTEXT_APPDATA_DIR environment variable
    (handy for debugging) or programmatically by the test runner. */
struct AppPaths
{
    static juce::File root();
    static juce::File settingsFile()        { return root().getChildFile ("settings.json"); }
    static juce::File licenseFile()         { return root().getChildFile ("license.json"); }
    static juce::File historyFile()         { return root().getChildFile ("history.json"); }
    static juce::File bridgeProfilesFile()  { return root().getChildFile ("bridge_profiles.json"); }
    static juce::File logsDir()             { return root().getChildFile ("Logs"); }
    static juce::File tempDir()             { return root().getChildFile ("Temp"); }
    static juce::File defaultGeneratedDir() { return root().getChildFile ("Generated"); }
    static juce::File contextCacheDir()     { return root().getChildFile ("ContextCache"); }

    /** Test hook: redirect everything to another folder. Call before any service starts. */
    static void setRootOverride (const juce::File& dir);
};
} // namespace context
