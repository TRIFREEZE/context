#pragma once

#include "GenerationModels.h"
#include <map>

namespace context
{
/** Describes how to talk to one kind of local generation bridge.
    DATA ONLY: paths, field names and mappings. Profiles can never contain code,
    scripts, hosts or credentials — the base URL comes from Settings.

    Loaded from the built-in profile and from %APPDATA%\Context\bridge_profiles.json.
    See docs/BRIDGE_PROFILES.md for the file format. */
struct BridgeConfiguration
{
    enum class RequestFormat { ContextV1, FieldMap };

    juce::String id, name;
    RequestFormat requestFormat = RequestFormat::ContextV1;

    // Endpoint paths (relative, start with '/'; "{id}" is replaced by the job ID)
    juce::String healthPath       = "/context/v1/health";
    juce::String capabilitiesPath = "/context/v1/capabilities";   // may be empty
    juce::String generatePath     = "/context/v1/generate";
    juce::String statusPath       = "/context/v1/jobs/{id}";
    juce::String cancelPath       = "/context/v1/jobs/{id}/cancel"; // may be empty
    juce::String downloadPath;                                      // optional: fetch audio here instead of the URL in status

    bool supportsPolling = true;            // false: generate returns the audio URL directly
    bool supportsCancellation = true;
    bool supportsAudioUpload = false;       // only meaningful for ContextV1
    bool healthRequiresContextProtocol = true;

    // Response field paths (dotted, e.g. "data.0.id")
    juce::String jobIdField    = "jobId";
    juce::String statusField   = "status";
    juce::String progressField = "progress";
    juce::String audioUrlField = "audioUrl";
    juce::String errorField    = "error";
    juce::String errorCodeField = "errorCode";

    /** Bridge status string (lower-case) -> Context state. Values limited to
        queued / generating / complete / failed / cancelled. */
    std::map<juce::String, JobState> statusMap;

    /** FieldMap only: Context request field -> bridge JSON key.
        Allowed source fields: prompt, userPrompt, type, durationSeconds, bpm, key, mode, instrumental, seed. */
    std::map<juce::String, juce::String> requestFields;

    /** FieldMap only: constant values added to every generate body (string/number/bool only). */
    juce::NamedValueSet staticFields;

    Capabilities staticCapabilities = Capabilities::allTypes();   // used when there is no capabilities endpoint

    int pollIntervalMs = 2000;
    int requestTimeoutMs = 15000;           // per HTTP call
    int syncGenerateTimeoutMs = 300000;     // when supportsPolling == false

    static BridgeConfiguration contextV1();             // the built-in default profile
    static const juce::StringArray& allowedRequestSourceFields();

    /** Parses one profile object. Returns an error message (and leaves `out` untouched) if invalid. */
    static juce::String fromVar (const juce::var&, BridgeConfiguration& out);

    /** Re-validates every value; returns the first problem or empty if valid. */
    juce::String validate() const;
};

/** Built-in profile + user profiles from a JSON file (invalid entries are skipped and reported). */
struct BridgeProfileSet
{
    std::vector<BridgeConfiguration> profiles;
    juce::StringArray problems;

    static BridgeProfileSet load (const juce::File& userProfilesFile);
    const BridgeConfiguration* find (const juce::String& id) const;
};

std::optional<JobState> jobStateFromProfileValue (const juce::String&);
} // namespace context
