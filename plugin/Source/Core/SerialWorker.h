#pragma once

#include <juce_core/juce_core.h>
#include <deque>
#include <functional>

namespace context
{
/** A background thread that runs queued tasks one at a time, in order.
    Exceptions thrown by tasks are caught and reported, never propagated.
    The destructor stops accepting work, lets the current task finish (tasks are
    expected to check their own cancellation) and joins the thread. */
class SerialWorker : private juce::Thread
{
public:
    explicit SerialWorker (const juce::String& name, int maxQueued = 64);
    ~SerialWorker() override;

    /** Returns false if the worker is shutting down or the queue is full. */
    bool post (std::function<void()> task);

    /** Drop all tasks that have not started yet. */
    void clearPending();

    /** Blocks until all queued tasks ran (tests / shutdown flush). */
    bool waitUntilIdle (int timeoutMs);

    int numPending() const;

    /** Called (on the worker thread) when a task throws. */
    std::function<void (const juce::String& what)> onTaskException;

private:
    void run() override;

    mutable juce::CriticalSection lock;
    std::deque<std::function<void()>> queue;
    juce::WaitableEvent wake;
    juce::WaitableEvent idle { true };
    bool busy = false;
    bool stopping = false;
    const int maxQueued;
};
} // namespace context
