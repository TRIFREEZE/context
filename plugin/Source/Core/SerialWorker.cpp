#include "SerialWorker.h"

namespace context
{
SerialWorker::SerialWorker (const juce::String& name, int maxQ)
    : juce::Thread (name), maxQueued (maxQ)
{
    idle.signal();
    startThread (juce::Thread::Priority::low);
}

SerialWorker::~SerialWorker()
{
    {
        const juce::ScopedLock sl (lock);
        stopping = true;
        queue.clear();
    }
    signalThreadShouldExit();
    wake.signal();
    // Tasks must honour their own cancellation tokens; 10 s is a last-resort bound.
    stopThread (10000);
}

bool SerialWorker::post (std::function<void()> task)
{
    {
        const juce::ScopedLock sl (lock);
        if (stopping || (int) queue.size() >= maxQueued)
            return false;
        queue.push_back (std::move (task));
        idle.reset();
    }
    wake.signal();
    return true;
}

void SerialWorker::clearPending()
{
    const juce::ScopedLock sl (lock);
    queue.clear();
    if (! busy)
        idle.signal();
}

int SerialWorker::numPending() const
{
    const juce::ScopedLock sl (lock);
    return (int) queue.size() + (busy ? 1 : 0);
}

bool SerialWorker::waitUntilIdle (int timeoutMs)
{
    return idle.wait (timeoutMs);
}

void SerialWorker::run()
{
    while (! threadShouldExit())
    {
        std::function<void()> task;
        {
            const juce::ScopedLock sl (lock);
            if (! queue.empty())
            {
                task = std::move (queue.front());
                queue.pop_front();
                busy = true;
            }
            else
            {
                busy = false;
                idle.signal();
            }
        }

        if (! task)
        {
            wake.wait (500);
            continue;
        }

        try
        {
            task();
        }
        catch (const std::exception& e)
        {
            if (onTaskException) onTaskException (e.what());
        }
        catch (...)
        {
            if (onTaskException) onTaskException ("unknown exception");
        }

        const juce::ScopedLock sl (lock);
        busy = false;
        if (queue.empty())
            idle.signal();
    }
}
} // namespace context
