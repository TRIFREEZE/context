#include "BridgeConfiguration.h"
#include "Utils/Json.h"
#include <tuple>

namespace context
{
namespace
{
    juce::String checkPath (const juce::String& name, const juce::String& p, bool optional)
    {
        if (p.isEmpty())
            return optional ? juce::String() : name + " is required";
        if (p.length() > 256)                          return name + " is too long";
        if (! p.startsWithChar ('/'))                  return name + " must start with '/'";
        if (p.startsWith ("//"))                       return name + " must not start with '//'";
        if (p.contains ("://") || p.containsChar ('@')) return name + " must be a path, not a URL";
        if (p.contains ("..") || p.containsAnyOf ("\\ \t\r\n#?\"'<>"))
            return name + " contains characters that are not allowed";
        const auto withoutId = p.replace ("{id}", "");
        if (withoutId.containsAnyOf ("{}"))            return name + " may only use the {id} placeholder";
        if (! withoutId.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_-.~"))
            return name + " contains characters that are not allowed";
        return {};
    }

    juce::String checkField (const juce::String& name, const juce::String& f, bool optional)
    {
        if (f.isEmpty())
            return optional ? juce::String() : name + " is required";
        return json::isValidFieldPath (f) ? juce::String() : name + " is not a valid field path";
    }
}

std::optional<JobState> jobStateFromProfileValue (const juce::String& v)
{
    const auto s = v.trim().toLowerCase();
    if (s == "queued")     return JobState::Queued;
    if (s == "generating") return JobState::Generating;
    if (s == "complete")   return JobState::Complete;
    if (s == "failed")     return JobState::Failed;
    if (s == "cancelled")  return JobState::Cancelled;
    return std::nullopt;
}

const juce::StringArray& BridgeConfiguration::allowedRequestSourceFields()
{
    static const juce::StringArray fields { "prompt", "userPrompt", "type", "durationSeconds", "bpm", "key", "mode", "instrumental", "seed" };
    return fields;
}

BridgeConfiguration BridgeConfiguration::contextV1()
{
    BridgeConfiguration c;
    c.id = "context-bridge-v1";
    c.name = "Context Bridge Protocol v1";
    c.requestFormat = RequestFormat::ContextV1;
    c.supportsAudioUpload = true;
    c.statusMap = { { "queued", JobState::Queued }, { "generating", JobState::Generating },
                    { "complete", JobState::Complete }, { "failed", JobState::Failed },
                    { "cancelled", JobState::Cancelled } };
    return c;
}

juce::String BridgeConfiguration::validate() const
{
    if (id.isEmpty() || id.length() > 64 || ! id.containsOnly ("abcdefghijklmnopqrstuvwxyz0123456789-_"))
        return "Profile id must be 1-64 lower-case letters, digits, '-' or '_'";
    if (name.length() > 100)
        return "Profile name is too long";

    using Check = std::tuple<const char*, juce::String, bool>;
    for (const auto& [n, p, opt] : std::initializer_list<Check> { { "healthPath", healthPath, false },
                              { "capabilitiesPath", capabilitiesPath, true }, { "generatePath", generatePath, false },
                              { "statusPath", statusPath, ! supportsPolling }, { "cancelPath", cancelPath, true },
                              { "downloadPath", downloadPath, true } })
        if (auto e = checkPath (n, p, opt); e.isNotEmpty())
            return e;

    if (supportsPolling && ! statusPath.contains ("{id}"))
        return "statusPath must contain {id}";
    if (supportsCancellation && cancelPath.isNotEmpty() && ! cancelPath.contains ("{id}"))
        return "cancelPath must contain {id}";

    for (const auto& [n, f, opt] : std::initializer_list<Check> { { "jobIdField", jobIdField, ! supportsPolling },
                              { "statusField", statusField, ! supportsPolling }, { "progressField", progressField, true },
                              { "audioUrlField", audioUrlField, downloadPath.isNotEmpty() }, { "errorField", errorField, true },
                              { "errorCodeField", errorCodeField, true } })
        if (auto e = checkField (n, f, opt); e.isNotEmpty())
            return e;

    if (supportsPolling && statusMap.empty())
        return "statusMap must map at least one status";
    for (auto& [k, v] : statusMap)
        if (k.isEmpty() || k.length() > 64)
            return "statusMap keys must be 1-64 characters";

    for (auto& [src, dst] : requestFields)
    {
        if (! allowedRequestSourceFields().contains (src))
            return "requestFields: unknown source field '" + src + "'";
        if (! json::isValidFieldPath (dst) || dst.containsChar ('.'))
            return "requestFields: '" + dst + "' is not a valid JSON key";
    }
    if (requestFormat == RequestFormat::FieldMap && requestFields.count ("prompt") == 0 && requestFields.count ("userPrompt") == 0)
        return "requestFields must map 'prompt' (or 'userPrompt')";

    for (auto& nv : staticFields)
    {
        if (! nv.value.isString() && ! nv.value.isBool() && ! nv.value.isInt() && ! nv.value.isInt64() && ! nv.value.isDouble())
            return "staticFields may only contain strings, numbers and booleans";
        if (nv.value.toString().length() > 256)
            return "staticFields values must be shorter than 256 characters";
    }

    if (pollIntervalMs < 250 || pollIntervalMs > 60000)            return "pollIntervalMs must be 250-60000";
    if (requestTimeoutMs < 1000 || requestTimeoutMs > 120000)      return "requestTimeoutMs must be 1000-120000";
    if (syncGenerateTimeoutMs < 5000 || syncGenerateTimeoutMs > 900000) return "syncGenerateTimeoutMs must be 5000-900000";
    return {};
}

juce::String BridgeConfiguration::fromVar (const juce::var& v, BridgeConfiguration& out)
{
    if (! json::isObject (v))
        return "Profile must be a JSON object";

    BridgeConfiguration c = contextV1();
    c.statusMap.clear();
    c.supportsAudioUpload = false;

    auto str = [&] (const char* k, juce::String& dst) { if (auto x = json::getString (v, k)) dst = x->trim(); };
    auto bl  = [&] (const char* k, bool& dst)         { if (auto x = json::getBool (v, k)) dst = *x; };
    auto num = [&] (const char* k, int& dst)          { if (auto x = json::getNumber (v, k)) dst = (int) *x; };

    str ("id", c.id);
    str ("name", c.name);
    if (c.name.isEmpty()) c.name = c.id;

    const auto fmt = json::getString (v, "requestFormat").value_or ("fieldmap").toLowerCase();
    if (fmt == "context-v1")       c.requestFormat = RequestFormat::ContextV1;
    else if (fmt == "fieldmap")    c.requestFormat = RequestFormat::FieldMap;
    else                           return "requestFormat must be 'context-v1' or 'fieldmap'";

    str ("healthPath", c.healthPath);
    str ("capabilitiesPath", c.capabilitiesPath);
    str ("generatePath", c.generatePath);
    str ("statusPath", c.statusPath);
    str ("cancelPath", c.cancelPath);
    str ("downloadPath", c.downloadPath);
    bl  ("supportsPolling", c.supportsPolling);
    bl  ("supportsCancellation", c.supportsCancellation);
    bl  ("supportsAudioUpload", c.supportsAudioUpload);
    c.healthRequiresContextProtocol = c.requestFormat == RequestFormat::ContextV1;
    bl  ("healthRequiresContextProtocol", c.healthRequiresContextProtocol);
    num ("pollIntervalMs", c.pollIntervalMs);
    num ("requestTimeoutMs", c.requestTimeoutMs);
    num ("syncGenerateTimeoutMs", c.syncGenerateTimeoutMs);

    const auto response = json::getPath (v, "response");
    if (json::isObject (response))
    {
        auto rs = [&] (const char* k, juce::String& dst) { if (auto x = json::getString (response, k)) dst = x->trim(); };
        rs ("jobIdPath", c.jobIdField);
        rs ("statusPath", c.statusField);
        rs ("progressPath", c.progressField);
        rs ("audioUrlPath", c.audioUrlField);
        rs ("errorPath", c.errorField);
        rs ("errorCodePath", c.errorCodeField);

        if (auto* map = json::getPath (response, "statusMap").getDynamicObject())
        {
            for (auto& nv : map->getProperties())
            {
                const auto state = jobStateFromProfileValue (nv.value.toString());
                if (! state)
                    return "statusMap value '" + nv.value.toString() + "' must be queued/generating/complete/failed/cancelled";
                c.statusMap[nv.name.toString().toLowerCase()] = *state;
            }
        }
    }
    if (c.statusMap.empty())
        c.statusMap = contextV1().statusMap;

    if (auto* fields = json::getPath (v, "requestFields").getDynamicObject())
        for (auto& nv : fields->getProperties())
            c.requestFields[nv.name.toString()] = nv.value.toString();

    if (auto* stat = json::getPath (v, "staticFields").getDynamicObject())
        c.staticFields = stat->getProperties();

    const auto caps = json::getPath (v, "capabilities");
    if (json::isObject (caps))
        c.staticCapabilities = Capabilities::fromVar (caps, Capabilities::allTypes());

    if (auto err = c.validate(); err.isNotEmpty())
        return err;

    out = c;
    return {};
}

BridgeProfileSet BridgeProfileSet::load (const juce::File& file)
{
    BridgeProfileSet set;
    set.profiles.push_back (BridgeConfiguration::contextV1());

    if (! file.existsAsFile())
        return set;

    if (file.getSize() > 512 * 1024)
    {
        set.problems.add (file.getFileName() + " is larger than 512 KB and was ignored");
        return set;
    }

    const auto parsed = json::parseUntrusted (file.loadFileAsString(), 512 * 1024, 16);
    if (! parsed.ok)
    {
        set.problems.add (file.getFileName() + ": " + parsed.error);
        return set;
    }

    auto* arr = json::getPath (parsed.value, "profiles").getArray();
    if (arr == nullptr)
    {
        set.problems.add (file.getFileName() + ": expected {\"profiles\": [ ... ]}");
        return set;
    }

    for (int i = 0; i < juce::jmin (arr->size(), 32); ++i)
    {
        BridgeConfiguration c;
        const auto err = BridgeConfiguration::fromVar (arr->getReference (i), c);
        if (err.isNotEmpty())
            set.problems.add ("Profile #" + juce::String (i + 1) + ": " + err);
        else if (set.find (c.id) != nullptr)
            set.problems.add ("Profile '" + c.id + "' is defined more than once; later copy ignored");
        else
            set.profiles.push_back (c);
    }
    return set;
}

const BridgeConfiguration* BridgeProfileSet::find (const juce::String& id) const
{
    for (auto& p : profiles)
        if (p.id == id)
            return &p;
    return nullptr;
}
} // namespace context
