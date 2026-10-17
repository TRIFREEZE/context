#pragma once

#include <juce_core/juce_core.h>
#include <deque>

namespace context
{
enum class LogLevel { Error = 0, Warning = 1, Info = 2, Debug = 3 };

/** Structured, redacting file logger. One per process (owned by SharedServices).
    Lines look like:  2026-10-16 20:41:03.512 INFO  [generation] job 3f2a... complete
    Every message passes through redaction::redact before it is written.
    NEVER call from the audio thread. */
class ContextLogger
{
public:
    explicit ContextLogger (juce::File logDirectory);
    ~ContextLogger();

    void setLevel (LogLevel l);
    LogLevel getLevel() const;

    void log (LogLevel level, const juce::String& category, const juce::String& message);

    /** Remember a short error code for "Copy Diagnostic Info" (keeps the last 20). */
    void recordErrorCode (const juce::String& code);
    juce::StringArray recentErrorCodes() const;

    juce::File currentLogFile() const { return logFile; }
    juce::File getLogDirectory() const { return dir; }

    /** Last N lines currently in memory (for the diagnostics view). */
    juce::StringArray recentLines (int maxLines = 50) const;

    static constexpr juce::int64 maxFileBytes = 2 * 1024 * 1024;
    static constexpr int filesToKeep = 3;

    /** Process-wide access for code that has no services pointer (may be null). */
    static ContextLogger* getInstance();

private:
    void rotateIfNeeded();

    juce::File dir, logFile;
    mutable juce::CriticalSection lock;
    std::unique_ptr<juce::FileOutputStream> stream;
    std::atomic<int> level { (int) LogLevel::Info };
    std::deque<juce::String> recentCodes, memoryLines;
};

/** Convenience helpers; safe no-ops if no logger exists yet. */
namespace logging
{
    void error   (const juce::String& category, const juce::String& message);
    void warning (const juce::String& category, const juce::String& message);
    void info    (const juce::String& category, const juce::String& message);
    void debug   (const juce::String& category, const juce::String& message);
}
} // namespace context
