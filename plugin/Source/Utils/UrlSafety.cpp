#include "UrlSafety.h"

namespace context::urls
{
namespace
{
    bool parseIPv4 (const juce::String& host, int out[4])
    {
        const auto parts = juce::StringArray::fromTokens (host, ".", "");
        if (parts.size() != 4)
            return false;
        for (int i = 0; i < 4; ++i)
        {
            if (parts[i].isEmpty() || parts[i].length() > 3 || ! parts[i].containsOnly ("0123456789"))
                return false;
            out[i] = parts[i].getIntValue();
            if (out[i] > 255)
                return false;
        }
        return true;
    }
}

bool isLoopbackHost (const juce::String& hostIn)
{
    const auto host = hostIn.toLowerCase().trimCharactersAtStart ("[").trimCharactersAtEnd ("]");
    if (host == "localhost" || host == "::1" || host == "0:0:0:0:0:0:0:1")
        return true;
    int ip[4];
    return parseIPv4 (host, ip) && ip[0] == 127;
}

HostKind classifyHost (const juce::String& hostIn)
{
    const auto host = hostIn.toLowerCase().trimCharactersAtStart ("[").trimCharactersAtEnd ("]");
    if (host.isEmpty())
        return HostKind::Invalid;
    if (isLoopbackHost (host))
        return HostKind::Loopback;

    int ip[4];
    if (parseIPv4 (host, ip))
    {
        if (ip[0] == 0)                                    return HostKind::Invalid;
        if (ip[0] == 10)                                   return HostKind::PrivateNetwork;
        if (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31)    return HostKind::PrivateNetwork;
        if (ip[0] == 192 && ip[1] == 168)                  return HostKind::PrivateNetwork;
        if (ip[0] == 169 && ip[1] == 254)                  return HostKind::PrivateNetwork;
        if (ip[0] == 100 && ip[1] >= 64 && ip[1] <= 127)   return HostKind::PrivateNetwork; // CGNAT / tailnets
        return HostKind::Public;
    }

    if (host.containsChar (':'))   // IPv6 literal
    {
        if (host.startsWith ("fc") || host.startsWith ("fd") || host.startsWith ("fe80"))
            return HostKind::PrivateNetwork;
        return HostKind::Public;
    }

    if (! host.containsOnly ("abcdefghijklmnopqrstuvwxyz0123456789-."))
        return HostKind::Invalid;

    if (host.endsWith (".local") || host.endsWith (".lan") || host.endsWith (".home") || ! host.containsChar ('.'))
        return HostKind::PrivateNetwork;

    return HostKind::Public;
}

ParsedUrl parseBaseUrl (const juce::String& textIn)
{
    ParsedUrl r;
    auto text = textIn.trim();

    if (text.isEmpty())                    { r.error = "Address is empty."; return r; }
    if (text.length() > 512)               { r.error = "Address is too long."; return r; }
    if (text.containsAnyOf (" \t\r\n\"<>\\")) { r.error = "Address contains invalid characters."; return r; }

    const int schemeEnd = text.indexOf ("://");
    if (schemeEnd <= 0)                    { r.error = "Address must start with http:// or https://"; return r; }

    r.scheme = text.substring (0, schemeEnd).toLowerCase();
    if (r.scheme != "http" && r.scheme != "https") { r.error = "Only http:// and https:// addresses are supported."; return r; }

    auto rest = text.substring (schemeEnd + 3);
    if (rest.containsAnyOf ("?#"))         { r.error = "Address must not contain a query (?) or fragment (#)."; return r; }

    const int slash = rest.indexOfChar ('/');
    const auto authority = slash >= 0 ? rest.substring (0, slash) : rest;
    const auto path = slash >= 0 ? rest.substring (slash) : juce::String();
    if (path.isNotEmpty() && path != "/")  { r.error = "Enter only the base address (for example http://127.0.0.1:3000) without a path."; return r; }
    if (authority.containsChar ('@'))      { r.error = "Addresses with usernames or passwords are not allowed."; return r; }
    if (authority.isEmpty())               { r.error = "Address has no host."; return r; }

    juce::String host, portText;
    if (authority.startsWithChar ('['))
    {
        const int close = authority.indexOfChar (']');
        if (close < 0)                     { r.error = "Malformed IPv6 address."; return r; }
        host = authority.substring (1, close);
        const auto after = authority.substring (close + 1);
        if (after.isNotEmpty())
        {
            if (! after.startsWithChar (':')) { r.error = "Malformed address."; return r; }
            portText = after.substring (1);
        }
    }
    else
    {
        if (authority.indexOfChar (':') != authority.lastIndexOfChar (':')) { r.error = "IPv6 addresses must be written in [brackets]."; return r; }
        host = authority.upToFirstOccurrenceOf (":", false, false);
        if (authority.containsChar (':'))
            portText = authority.fromFirstOccurrenceOf (":", false, false);
    }

    host = host.toLowerCase();
    if (host.isEmpty())                    { r.error = "Address has no host."; return r; }

    if (portText.isNotEmpty())
    {
        if (! portText.containsOnly ("0123456789") || portText.length() > 5) { r.error = "Port must be a number."; return r; }
        r.port = portText.getIntValue();
        if (r.port < 1 || r.port > 65535) { r.error = "Port must be between 1 and 65535."; return r; }
    }
    else
    {
        r.port = r.scheme == "https" ? 443 : 80;
    }

    r.kind = classifyHost (host);
    if (r.kind == HostKind::Invalid)       { r.error = "Host name is not valid."; return r; }

    r.host = host;
    const auto hostPart = host.containsChar (':') ? "[" + host + "]" : host;
    r.normalized = r.scheme + "://" + hostPart + (portText.isNotEmpty() ? ":" + juce::String (r.port) : juce::String());
    r.ok = true;
    return r;
}

juce::String joinPath (const juce::String& normalizedBase, const juce::String& path, const juce::String& jobId)
{
    auto p = path;
    if (jobId.isNotEmpty())
        p = p.replace ("{id}", juce::URL::addEscapeChars (jobId, false));
    if (! p.startsWithChar ('/'))
        p = "/" + p;
    return normalizedBase + p;
}

DownloadUrlCheck checkDownloadUrl (const juce::String& candidateIn, const ParsedUrl& bridgeBase)
{
    DownloadUrlCheck c;
    const auto candidate = candidateIn.trim();

    if (candidate.isEmpty() || candidate.length() > 4096 || candidate.containsAnyOf (" \t\r\n\"<>\\"))
    {
        c.error = "Bridge returned an invalid audio URL.";
        return c;
    }

    if (candidate.startsWithChar ('/') && ! candidate.startsWith ("//"))
    {
        if (candidate.contains ("/../") || candidate.endsWith ("/.."))
        {
            c.error = "Bridge returned an invalid relative audio path.";
            return c;
        }
        c.url = bridgeBase.normalized + candidate;
        c.ok = true;
        return c;
    }

    const int schemeEnd = candidate.indexOf ("://");
    const auto scheme = schemeEnd > 0 ? candidate.substring (0, schemeEnd).toLowerCase() : juce::String();
    if (scheme != "http" && scheme != "https")
    {
        c.error = "Audio URL uses an unsupported scheme (only http/https are allowed).";
        return c;
    }

    const auto afterScheme = candidate.substring (schemeEnd + 3);
    auto authority = afterScheme.upToFirstOccurrenceOf ("/", false, false).upToFirstOccurrenceOf ("?", false, false);
    if (authority.containsChar ('@'))
    {
        c.error = "Audio URL must not contain credentials.";
        return c;
    }
    const auto host = authority.startsWithChar ('[') ? authority.fromFirstOccurrenceOf ("[", false, false).upToFirstOccurrenceOf ("]", false, false)
                                                     : authority.upToFirstOccurrenceOf (":", false, false);

    if (scheme == "http" && ! isLoopbackHost (host) && host.toLowerCase() != bridgeBase.host)
    {
        c.error = "Refusing to download audio over unencrypted http from a non-local host.";
        return c;
    }

    c.url = candidate;
    c.ok = true;
    return c;
}

juce::String describe (HostKind k)
{
    switch (k)
    {
        case HostKind::Loopback:       return "this computer";
        case HostKind::PrivateNetwork: return "another device on your local network";
        case HostKind::Public:         return "the public internet";
        case HostKind::Invalid:        break;
    }
    return "an invalid host";
}
} // namespace context::urls
