# Bridge profiles

A bridge profile tells Context how to talk to a bridge whose API is **not** the
Context Bridge Protocol. Profiles are plain JSON configuration — paths, field
names and value mappings. They can never contain code, scripts, host names or
credentials, and every value is validated before use.

Location: `%APPDATA%\Context\bridge_profiles.json`
(Settings → Generation → *Edit Profiles...* creates an example and opens the folder.)

After editing, reopen Settings: valid profiles appear in the *Bridge profile*
menu; problems are listed under the bridge address.

## Example

```json
{
  "profiles": [
    {
      "id": "my-bridge",
      "name": "My bridge",
      "requestFormat": "fieldmap",
      "healthPath": "/health",
      "capabilitiesPath": "",
      "generatePath": "/api/generate",
      "statusPath": "/api/jobs/{id}",
      "cancelPath": "/api/jobs/{id}/cancel",
      "downloadPath": "",
      "supportsPolling": true,
      "supportsCancellation": true,
      "pollIntervalMs": 2000,
      "requestTimeoutMs": 15000,
      "requestFields": { "prompt": "text", "bpm": "tempo", "durationSeconds": "duration" },
      "staticFields":  { "model": "default" },
      "response": {
        "jobIdPath": "data.0.id",
        "statusPath": "data.0.status",
        "progressPath": "data.0.progress",
        "audioUrlPath": "data.0.audio_url",
        "errorPath": "error.message",
        "statusMap": { "queued": "queued", "running": "generating", "done": "complete", "error": "failed" }
      },
      "capabilities": { "generationTypes": ["one_shot", "loop", "vocal"], "features": ["cancellation"] }
    }
  ]
}
```

## Fields

| Field | Meaning | Rules |
|---|---|---|
| `id` | Stable identifier | 1–64 chars: `a-z 0-9 - _` |
| `requestFormat` | `fieldmap` (use `requestFields`) or `context-v1` | |
| `healthPath` | Checked by *Test Connection* | starts with `/`, no `..`, no `://`, no `?#` |
| `capabilitiesPath` | Optional capabilities endpoint | same; may be empty |
| `generatePath` | POST target | required |
| `statusPath` | GET for polling | must contain `{id}` when polling |
| `cancelPath` / `downloadPath` | Optional, may use `{id}` | |
| `supportsPolling` | `false` = generate returns the finished audio URL directly | |
| `requestFields` | Context field → bridge JSON key | sources: `prompt`, `userPrompt`, `type`, `durationSeconds`, `bpm`, `key`, `mode`, `instrumental`, `seed` |
| `staticFields` | Constant values added to every request | strings, numbers, booleans only |
| `response.*Path` | Dotted paths into the JSON reply | segments `A-Z a-z 0-9 _ -`, numbers index arrays |
| `response.statusMap` | Bridge status → Context state | values: `queued`, `generating`, `complete`, `failed`, `cancelled` |
| `capabilities` | Used when there is no capabilities endpoint | same shape as the protocol's capabilities response |
| `pollIntervalMs` | 250–60000 | |
| `requestTimeoutMs` | 1000–120000 | |

The `{id}` placeholder is URL-encoded, so a job ID can never change the path.

## Connecting an existing bridge

1. Read your bridge's own documentation for its health, generate and status endpoints.
2. Write a profile that maps them. **Do not guess** endpoint names: if the bridge
   doesn't document something, leave that feature off.
3. Settings → Generation → choose the profile → *Test Connection*.

Context ships with only the built-in `context-bridge-v1` profile; it does not
include profiles for any third-party bridge, because their APIs are not documented here.
