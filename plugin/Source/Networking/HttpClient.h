#pragma once

#include "Utils/CancellationToken.h"

namespace context
{
/** Minimal blocking HTTP client for worker threads, built on juce::WebInputStream.
    - hard response-size limit (protects against huge/endless responses)
    - connect/read timeout
    - cancellation via CancellationToken (aborts an in-flight request)
    - never throws */
class HttpClient
{
public:
    struct Request
    {
        juce::String url;
        juce::String method = "GET";            // GET | POST | DELETE
        juce::String jsonBody;                  // sent for POST
        juce::StringPairArray headers;
        int timeoutMs = 10000;
        juce::int64 maxResponseBytes = 2 * 1024 * 1024;
    };

    struct Response
    {
        bool connected = false;                 // false: DNS/refused/timeout/cancelled before a response
        int statusCode = 0;
        juce::MemoryBlock body;
        bool truncated = false;                 // response exceeded maxResponseBytes (body discarded)
        bool cancelled = false;
        int elapsedMs = 0;
        juce::String transportError;

        bool isSuccess() const   { return connected && statusCode >= 200 && statusCode < 300 && ! truncated && ! cancelled; }
        juce::String bodyText() const { return juce::String::fromUTF8 ((const char*) body.getData(), (int) body.getSize()); }
    };

    static Response send (const Request&, CancellationToken& token);

    /** Streams the body of a GET into `destination`, enforcing `maxBytes`. */
    struct FileResponse
    {
        bool connected = false;
        int statusCode = 0;
        juce::int64 bytes = 0;
        bool tooLarge = false;
        bool cancelled = false;
        bool writeFailed = false;
        juce::String transportError;
        juce::String contentType;
    };
    static FileResponse downloadToFile (const juce::String& url, const juce::File& destination,
                                        juce::int64 maxBytes, int timeoutMs, CancellationToken& token);

    static juce::String userAgent();
};
} // namespace context
