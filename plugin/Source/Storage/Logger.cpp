#include "Logger.h"
#include "Utils/Redaction.h"

namespace context
{
namespace
{
    std::atomic<ContextLogger*> globalLogger { nullptr };

    const char* levelName (LogLevel l)
    {
        switch (l)
        {
            case LogLevel::Error:   return "ERROR";
            case LogLevel::Warning: return "WARN ";
            case LogLevel::Info:    return "INFO ";
            case LogLevel::Debug:   return "DEBUG";
        }
        return "INFO ";
    }
}

ContextLogger* ContextLogger::getInstance() { return globalLogger.load(); }

ContextLogger::ContextLogger (juce::File logDirectory)
    : dir (std::move (logDirectory))
{
    dir.createDirectory();
    logFile = dir.getChildFile ("context.log");
    rotateIfNeeded();
    stream = std::make_unique<juce::FileOutputStream> (logFile);
    if (! stream->openedOk())
        stream.reset();   // logging is best-effort; an unwritable folder must not break the plugin
    globalLogger.store (this);
}

ContextLogger::~ContextLogger()
{
    ContextLogger* expected = this;
    globalLogger.compare_exchange_strong (expected, nullptr);
    const juce::ScopedLock sl (lock);
    if (stream != nullptr)
        stream->flush();
}

void ContextLogger::setLevel (LogLevel l) { level.store ((int) l); }
LogLevel ContextLogger::getLevel() const  { return (LogLevel) level.load(); }

void ContextLogger::rotateIfNeeded()
{
    if (logFile.getSize() < maxFileBytes)
        return;

    stream.reset();
    for (int i = filesToKeep - 1; i >= 1; --i)
    {
        auto older = dir.getChildFile ("context." + juce::String (i) + ".log");
        auto newer = i == 1 ? logFile : dir.getChildFile ("context." + juce::String (i - 1) + ".log");
        if (newer.existsAsFile())
        {
            older.deleteFile();
            newer.moveFileTo (older);
        }
    }
    stream = std::make_unique<juce::FileOutputStream> (logFile);
    if (! stream->openedOk())
        stream.reset();
}

void ContextLogger::log (LogLevel l, const juce::String& category, const juce::String& message)
{
    if ((int) l > level.load())
        return;

    const auto safe = redaction::redact (message).replaceCharacters ("\r\n", "  ");
    const auto line = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S")
                    + "." + juce::String (juce::Time::getCurrentTime().getMilliseconds()).paddedLeft ('0', 3)
                    + " " + levelName (l) + " [" + category + "] " + safe;

    const juce::ScopedLock sl (lock);
    memoryLines.push_back (line);
    while (memoryLines.size() > 200)
        memoryLines.pop_front();

    if (stream == nullptr)
        return;
    if (stream->getPosition() > maxFileBytes)
        rotateIfNeeded();
    if (stream == nullptr)
        return;
    stream->writeText (line + juce::newLine, false, false, nullptr);
    stream->flush();
}

void ContextLogger::recordErrorCode (const juce::String& code)
{
    const juce::ScopedLock sl (lock);
    recentCodes.push_back (juce::Time::getCurrentTime().formatted ("%H:%M:%S ") + code);
    while (recentCodes.size() > 20)
        recentCodes.pop_front();
}

juce::StringArray ContextLogger::recentErrorCodes() const
{
    const juce::ScopedLock sl (lock);
    juce::StringArray a;
    for (auto& c : recentCodes)
        a.add (c);
    return a;
}

juce::StringArray ContextLogger::recentLines (int maxLines) const
{
    const juce::ScopedLock sl (lock);
    juce::StringArray a;
    const int start = juce::jmax (0, (int) memoryLines.size() - maxLines);
    for (int i = start; i < (int) memoryLines.size(); ++i)
        a.add (memoryLines[(size_t) i]);
    return a;
}

namespace logging
{
    void error   (const juce::String& c, const juce::String& m) { if (auto* l = ContextLogger::getInstance()) l->log (LogLevel::Error, c, m); }
    void warning (const juce::String& c, const juce::String& m) { if (auto* l = ContextLogger::getInstance()) l->log (LogLevel::Warning, c, m); }
    void info    (const juce::String& c, const juce::String& m) { if (auto* l = ContextLogger::getInstance()) l->log (LogLevel::Info, c, m); }
    void debug   (const juce::String& c, const juce::String& m) { if (auto* l = ContextLogger::getInstance()) l->log (LogLevel::Debug, c, m); }
}
} // namespace context
