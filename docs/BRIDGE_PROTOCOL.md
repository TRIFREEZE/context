# Context Bridge Protocol v1

This is the HTTP/JSON contract that Context's built-in bridge profile speaks.
It is **defined by this project** — it is not the API of Suno or of any other
service, and Context makes no assumption about how a bridge talks to its own
upstream provider.

A *bridge* is a small program the user runs on their own computer. It accepts
requests from Context on `http://127.0.0.1:<port>`, does whatever it needs to
generate audio (including its own sign-in with a provider), and hands back a
WAV file. Context never sees provider credentials.

A working reference implementation with synthetic audio lives in
`tools/reference_bridge/reference_bridge.py` (standard-library Python only).

> Bridges with a different API don't need to implement this protocol: describe
> them with a bridge profile instead (see `BRIDGE_PROFILES.md`).

---

## Conventions

* Base URL: `http://127.0.0.1:3000` by default (configurable in Context).
* Bind to `127.0.0.1` only. Context refuses non-local bridges unless the user
  explicitly allows them.
* Request and response bodies are UTF-8 JSON. Context sends `Content-Type: application/json`.
* Keep responses under 2 MB (JSON) and audio under 100 MB. Context rejects larger ones.
* Context sends **no** cookies, authorization headers or credentials to the bridge.

## Endpoints

### `GET /context/v1/health`

```json
{ "protocol": "context-bridge-v1", "name": "My Bridge", "version": "1.2.0" }
```
`protocol` must be exactly `context-bridge-v1`; otherwise Context reports *Unsupported API*.

### `GET /context/v1/capabilities` (recommended)

```json
{
  "generationTypes": ["one_shot", "loop", "vocal", "instrument", "percussion", "texture", "fx", "melody"],
  "features": ["variation", "cancellation", "audio_conditioning"],
  "maxDurationSeconds": 30,
  "contextModes": ["layer", "cover", "extend"],
  "maxAudioContextSeconds": 120
}
```
`contextModes` (optional) says how the bridge can use the track audio:

| Mode | Meaning | Result timing |
|---|---|---|
| `layer` | make a part that plays **on top of** the track context (same tempo, key, structure) | sample 0 = bar 1 of the context; Context keeps it exactly in place |
| `cover` | re-imagine the track context in the prompt's style | free; Context fits it like any other sound |
| `extend` | generate what comes **after** the track context | sample 0 = the bar after the context; kept in place |

Bridges that list `audio_conditioning` but no `contextModes` are treated as `layer` only
(plus `cover` / `extend` when the `cover` / `continuation` features are listed).
Feature ids: `full_song`, `audio_conditioning`, `continuation`, `variation`,
`cover`, `stem_generation`, `cancellation`. Context disables UI controls for
anything not listed (e.g. the *Variation* button, *Send Audio Context*).

### `POST /context/v1/generate`

Request:
```json
{
  "protocol": "context-bridge-v1",
  "requestId": "0b6f...-uuid",
  "prompt": "Dark distorted female vocal chop. Designed for a 142 BPM track in F# minor. ...",
  "userPrompt": "dark distorted female vocal chop",
  "type": "vocal",
  "durationSeconds": 4.0,
  "bpm": 142,
  "key": "F#",
  "mode": "minor",
  "instrumental": false,
  "seed": 1234567,
  "variationOf": "<jobId of an earlier generation>",
  "context": { "bpm": 142, "key": "F#", "mode": "minor", "lowEnd": "High", "mids": "Medium",
               "highs": "Low", "stereoWidth": "Wide", "transientDensity": "Moderate", "tonalBalance": "Dark" },
  "audioContext": { "format": "wav", "encoding": "base64", "data": "<base64 WAV>", "seconds": 13.714, "bars": 8 },
  "contextMode": "layer",
  "negativeTags": "busy, cluttered, dense"
}
```
* `prompt` is what to generate from (context-enhanced unless the user turned enhancement off); `userPrompt` is exactly what they typed.
* Optional fields are omitted when unknown.
* `audioContext` is sent **only** if the bridge lists `audio_conditioning` *and* the
  user allowed it (Settings, or the consent sheet) *and* turned on **Send my track**.
  It is the captured context (exactly `bars` bars, 44.1 kHz stereo 16-bit, at most 90 s)
  when one exists, otherwise the last 8 s the plugin heard (mono 22.05 kHz, no `bars`).
  The WAV is at most 16 MB.
* `contextMode` is one of the bridge's `contextModes`; only sent together with `audioContext`.
* `negativeTags` (optional, ≤ 200 characters, comma-separated): what the producer's **Pass**
  verdicts on this prompt said to avoid. Bridges pass it to models that accept negative tags and
  may ignore it otherwise.

Response (`200` or `202`):
```json
{ "jobId": "a1b2c3" }
```
Job IDs: 1–128 characters from `A-Z a-z 0-9 - _ . : ~`.

Errors: a non-2xx status with
```json
{ "error": "human readable message", "errorCode": "invalid_request" }
```

### `GET /context/v1/jobs/{jobId}`

```json
{ "jobId": "a1b2c3", "status": "generating", "progress": 0.42 }
{ "jobId": "a1b2c3", "status": "complete", "audioUrl": "/files/a1b2c3.wav" }
{ "jobId": "a1b2c3", "status": "failed", "error": "content policy", "errorCode": "rejected" }
```
* `status`: `queued`, `generating`, `complete`, `failed`, `cancelled`.
* `progress`: 0–1 (or 0–100), optional.
* `audioUrl`: a path on the bridge (`/files/x.wav`), an `http://127.0.0.1/...` URL, or
  any `https://` URL. Plain `http://` to other hosts is refused.
* Context polls every 2 s (with backoff on errors) until done or the timeout (default 10 min).

### `POST /context/v1/jobs/{jobId}/cancel` (optional)

Any 2xx response is fine.

## Error codes Context understands

| `errorCode` contains | Shown to the user as |
|---|---|
| `auth`, `session`, `login`, `credential` | "Your local bridge reports its upstream sign-in has expired. Re-authenticate in the bridge." |
| `rate`, `quota`, `busy` | "The provider is busy or rate-limited." |
| `invalid`, `bad_request` | "That request can't be generated." |
| `unsupported` | "This provider doesn't support that feature." |

HTTP 401/403 from the bridge are also reported as an expired upstream sign-in;
404 on the status endpoint means the bridge forgot the job (e.g. restarted).

## Audio

WAV (preferred) or AIFF, any common sample rate (8–384 kHz), 1–8 channels,
up to 20 minutes and 100 MB. Context verifies every file with a real decoder.
