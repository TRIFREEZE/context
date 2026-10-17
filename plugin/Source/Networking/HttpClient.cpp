#include "HttpClient.h"
#include "BuildInfo.h"

namespace context
{
juce::String HttpClient::userAgent()
{
    return juce::String (build::productName) + "/" + build::version;
}

namespace
{
    juce::String buildHeaders (const juce::StringPairArray& extra, bool hasJsonBody)
    {
        juce::String h;
        h << "User-Agent: " << HttpClient::userAgent() << "\r\n";
        h << "Accept: application/json, audio/wav, */*\r\n";
        if (hasJsonBody)
            h << "Content-Type: application/json\r\n";
        for (const auto& key : extra.getAllKeys())
        {
            // Header injection guard: no CR/LF in names or values.
            const auto v = extra[key];
            if (key.containsAnyOf ("\r\n:") || v.containsAnyOf ("\r\n"))
                continue;
            h << key << ": " << v << "\r\n";
        }
        return h;
    }
}

HttpClient::Response HttpClient::send (const Request& req, CancellationToken& token)
{
    Response r;
    const auto start = juce::Time::getMillisecondCounter();

    if (token.isCancelled())
    {
        r.cancelled = true;
        return r;
    }

    try
    {
        const bool isPost = req.method.equalsIgnoreCase ("POST");
        juce::URL url (req.url);
        if (isPost)
            url = url.withPOSTData (req.jsonBody);

        juce::WebInputStream stream (url, isPost);
        stream.withExtraHeaders (buildHeaders (req.headers, isPost));
        stream.withConnectionTimeout (juce::jlimit (500, 120000, req.timeoutMs));
        stream.withNumRedirectsToFollow (3);
        if (! isPost && ! req.method.equalsIgnoreCase ("GET"))
            stream.withCustomRequestCommand (req.method.toUpperCase());

        CancellationToken::StreamScope scope (&token, stream);

        r.connected = stream.connect (nullptr);
        r.statusCode = stream.getStatusCode();
        if (token.isCancelled())
        {
            r.cancelled = true;
            r.connected = false;
        }
        else if (! r.connected || r.statusCode == 0)
        {
            r.connected = false;
            r.transportError = "No response from " + url.getOrigin();
        }
        else
        {
            const auto declared = stream.getTotalLength();
            if (declared > req.maxResponseBytes)
            {
                r.truncated = true;
            }
            else
            {
                char buffer[16384];
                while (! stream.isExhausted())
                {
                    if (token.isCancelled()) { r.cancelled = true; break; }
                    const int n = stream.read (buffer, (int) sizeof (buffer));
                    if (n <= 0)
                        break;
                    if ((juce::int64) r.body.getSize() + n > req.maxResponseBytes)
                    {
                        r.truncated = true;
                        r.body.reset();
                        break;
                    }
                    r.body.append (buffer, (size_t) n);
                }
                if (stream.isError() && ! r.cancelled && ! r.truncated && r.body.isEmpty())
                    r.transportError = "Connection dropped while reading the response";
            }
        }
    }
    catch (const std::exception& e)
    {
        r.connected = false;
        r.transportError = juce::String ("HTTP exception: ") + e.what();
    }
    catch (...)
    {
        r.connected = false;
        r.transportError = "HTTP exception";
    }

    r.elapsedMs = (int) (juce::Time::getMillisecondCounter() - start);
    return r;
}

HttpClient::FileResponse HttpClient::downloadToFile (const juce::String& urlText, const juce::File& destination,
                                                     juce::int64 maxBytes, int timeoutMs, CancellationToken& token)
{
    FileResponse r;
    if (token.isCancelled()) { r.cancelled = true; return r; }

    try
    {
        juce::URL url (urlText);
        juce::WebInputStream stream (url, false);
        stream.withExtraHeaders (buildHeaders ({}, false));
        stream.withConnectionTimeout (juce::jlimit (1000, 300000, timeoutMs));
        stream.withNumRedirectsToFollow (5);
        CancellationToken::StreamScope scope (&token, stream);

        r.connected = stream.connect (nullptr);
        r.statusCode = stream.getStatusCode();
        if (token.isCancelled()) { r.cancelled = true; r.connected = false; return r; }
        if (! r.connected || r.statusCode == 0) { r.connected = false; r.transportError = "No response from " + url.getOrigin(); return r; }
        r.contentType = stream.getResponseHeaders()["Content-Type"];
        if (r.statusCode < 200 || r.statusCode >= 300)
            return r;

        if (stream.getTotalLength() > maxBytes) { r.tooLarge = true; return r; }

        destination.deleteFile();
        juce::FileOutputStream out (destination);
        if (! out.openedOk()) { r.writeFailed = true; return r; }

        juce::HeapBlock<char> buffer (65536);
        while (! stream.isExhausted())
        {
            if (token.isCancelled()) { r.cancelled = true; break; }
            const int n = stream.read (buffer.get(), 65536);
            if (n <= 0)
                break;
            r.bytes += n;
            if (r.bytes > maxBytes) { r.tooLarge = true; break; }
            if (! out.write (buffer.get(), (size_t) n)) { r.writeFailed = true; break; }
        }
        out.flush();
        if (out.getStatus().failed())
            r.writeFailed = true;
        if (stream.isError() && ! r.cancelled && ! r.tooLarge)
            r.transportError = "Connection dropped during download";
    }
    catch (const std::exception& e)
    {
        r.transportError = juce::String ("Download exception: ") + e.what();
    }
    catch (...)
    {
        r.transportError = "Download exception";
    }

    if (r.cancelled || r.tooLarge || r.writeFailed || r.transportError.isNotEmpty())
        destination.deleteFile();
    return r;
}
} // namespace context
