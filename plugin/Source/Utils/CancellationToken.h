#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>

namespace context
{
/** Shared cancellation flag. Cancelling also aborts any registered in-flight
    WebInputStream so a blocked HTTP call returns immediately. Thread-safe. */
class CancellationToken
{
public:
    using Ptr = std::shared_ptr<CancellationToken>;
    static Ptr create() { return std::make_shared<CancellationToken>(); }

    void cancel()
    {
        cancelled.store (true);
        const juce::ScopedLock sl (lock);
        if (activeStream != nullptr)
            activeStream->cancel();
        wakeUp.signal();
    }

    bool isCancelled() const noexcept { return cancelled.load(); }

    /** Sleeps up to ms, returning early (true) if cancelled. */
    bool sleepOrCancelled (int ms)
    {
        if (isCancelled())
            return true;
        wakeUp.wait (ms);
        return isCancelled();
    }

    /** RAII registration of the stream currently doing I/O for this token. */
    class StreamScope
    {
    public:
        StreamScope (CancellationToken* t, juce::WebInputStream& s) : token (t)
        {
            if (token != nullptr)
            {
                const juce::ScopedLock sl (token->lock);
                token->activeStream = &s;
                if (token->isCancelled())
                    s.cancel();
            }
        }
        ~StreamScope()
        {
            if (token != nullptr)
            {
                const juce::ScopedLock sl (token->lock);
                token->activeStream = nullptr;
            }
        }
    private:
        CancellationToken* token;
        JUCE_DECLARE_NON_COPYABLE (StreamScope)
    };

private:
    std::atomic<bool> cancelled { false };
    juce::CriticalSection lock;
    juce::WebInputStream* activeStream = nullptr;
    juce::WaitableEvent wakeUp { true };
};
} // namespace context
