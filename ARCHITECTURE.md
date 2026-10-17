# Context — Architecture

This document describes how Context is put together, who owns what, and which
thread is allowed to do what. It was written before implementation (Phase 2)
and kept in sync with the code afterwards.

---

## 1. High-level system

```
                 ┌──────────────────────────── user's computer ─────────────────────────────┐
                 │                                                                          │
  DAW audio ───► │  Context.vst3 ──(HTTP/JSON, localhost)──► user-run generation bridge ────┼──► provider
                 │      │                                                                   │
                 │      ├── local files: settings, history, cache, logs (%APPDATA%\Context) │
                 │      │                                                                   │
                 └──────┼───────────────────────────────────────────────────────────────────┘
                        │ HTTPS (license key + hashed device ID + version only)
                        ▼
                 Context licensing backend (FastAPI + SQLite)
```

Two **completely separate** network paths:

| Path | Carries | Never carries |
|---|---|---|
| Plugin → generation bridge (localhost) | prompts, expanded prompts, generation options, (opt-in) audio context | license key, provider credentials |
| Plugin → licensing backend | license key, hashed device ID, plugin version | prompts, audio, provider credentials, cookies, tokens of any provider |

Context never touches browser profiles, cookies or provider credentials. The
bridge owns its own upstream authentication; to Context it is just a REST API.

---

## 2. Source layout

```
plugin/Source/
  PluginProcessor.*      audio entry point; owns per-instance services
  PluginEditor.*         UI root; owns nothing the processor needs
  Core/                  build info, product config, app paths, settings, shared services, worker thread
  Utils/                 filename sanitizer, atomic file writes, JSON safety, URL safety, redaction
  Audio/                 CaptureBuffer (rolling ring buffer), PreviewPlayer
  Analysis/              TrackAnalyzer (pure DSP functions), AnalysisController (worker)
  Generation/            request/result models, IAudioGenerationProvider, Mock + LocalBridge providers,
                         BridgeConfiguration/BridgeApiAdapter, PromptBuilder, SynthAudio, GenerationController
  Networking/            HttpClient (cancellable, size-limited), Downloader (temp file → validate → rename)
  Licensing/             DeviceId, LicenseToken (Ed25519 verify), LicenseManager
  Storage/               Logger, HistoryStore, CacheManager
  Security/              compiled-in public key handling
  UI/                    look-and-feel and all components
third_party/monocypher   Ed25519 signature verification (vendored, BSD-2/CC0)
backend/                 licensing server + admin CLI + tests
tools/reference_bridge   stdlib-only Python test bridge implementing the Context Bridge Protocol
tests/                   C++ unit tests + bridge integration test driver
```

All core logic lives in plain C++ classes that do not depend on the editor, so
the test executable compiles the same sources the plugin uses.

---

## 3. Object ownership

```
ContextAudioProcessor (one per plugin instance, owned by host)
 ├── SharedResourcePointer<SharedServices>   one per process, ref-counted across instances
 │     ├── SettingsStore      (settings.json)
 │     ├── Logger             (Logs/context.log)
 │     ├── HistoryStore       (history.json)  + CacheManager
 │     ├── LicenseManager     (license.json, licensing worker thread)
 │     └── FileWorker         (serial background thread for history/cache I/O)
 ├── CaptureBuffer            preallocated ring, written by audio thread
 ├── HostInfo atomics         BPM / playing / PPQ / time signature
 ├── PreviewPlayer            realtime-safe clip handoff
 ├── AnalysisController       owns analysis worker thread
 └── GenerationController     owns provider + generation worker thread + job queue

ContextAudioProcessorEditor (created/destroyed freely by host)
 └── UI components — read state from controllers, register as ChangeListeners,
     unregister in destructors. The editor owns NO jobs, threads or files.
```

Rules:
* No raw owning pointers. `std::unique_ptr` for exclusive ownership, `SharedResourcePointer`
  for process-wide services.
* Worker threads are members of the controller that uses them and are joined in
  that controller's destructor, *before* any member they touch is destroyed.
* Cross-thread notifications use `juce::ChangeBroadcaster::sendChangeMessage()`
  (async, coalesced, delivered on the message thread). Listeners remove themselves
  in their destructors, so a closed editor can never receive a callback.
* Workers never capture the editor. They only touch their owning controller.

---

## 4. Thread ownership

| Thread | Allowed | Forbidden |
|---|---|---|
| **Audio thread** (`processBlock`) | passthrough (no-op), peak check, write into CaptureBuffer under `SpinLock::tryLock` (skips block if busy, never waits), read play-head into atomics, mix preview clip (already decoded, pre-allocated), atomic flag reads | HTTP, disk I/O, JSON, licensing, allocation, logging, FFT, waiting on locks, sleeping |
| **Message thread** | UI, settings edits, starting jobs, file choosers, drag-and-drop, reveal-in-explorer | blocking network, long file I/O, FFT |
| **Generation worker** (1 per instance) | provider calls: test connection, submit, poll, cancel, download, decode-validate, final rename | touching UI components |
| **Analysis worker** (1 per instance) | snapshot capture buffer (tryLock-free copy under SpinLock), FFT, key/spectral/stereo/transient analysis | network |
| **File worker** (1 per process) | history writes, cache enforcement, settings writes | network |
| **Licensing worker** (1 per process) | activation, validation, update check | touching audio |
| **Preview loader** (generation/file worker task) | decode WAV into memory clip before handing to audio thread | — |

Deadlock avoidance:
* The audio thread only ever *try*-locks; it cannot be part of a deadlock.
* Controller state mutexes are leaf locks: no code calls out (to another lock,
  provider, or listener) while holding one. `sendChangeMessage()` is called after
  the lock is released.
* Workers are joined with a timeout on shutdown; long operations are cancelled
  first via `CancellationToken` (which also aborts an in-flight `WebInputStream`).

---

## 5. Audio flow and rolling buffer

```
input ──► [capture: copy (pre-preview) to ring if block not silent] ──► output (unchanged)
                                                                  └─► + preview clip × gain (realtime only, not during offline render)
```

* Passthrough is bit-exact: Context never writes to the main buffer except to
  *add* the preview clip while the user is actively previewing.
* The capture happens **before** preview mixing, so previews never contaminate
  analysis (no feedback loop).
* CaptureBuffer: stereo ring of `captureSeconds × sampleRate` frames, allocated in
  `prepareToPlay` (message thread) or when the capture-length setting changes.
  The swap happens under the same SpinLock; if the audio thread finds it locked it
  skips writing that block (capture is best-effort, passthrough is never affected).
* Silent blocks (peak < -100 dBFS) are not written, so stopping the transport keeps
  the last N seconds of real music available for analysis.
* During offline rendering (`isNonRealtime()`), preview is never mixed in.

---

## 6. Analysis flow

1. User clicks **Analyze Track** (message thread). If an analysis is running the
   click is ignored (coalesced).
2. Analysis worker copies the ring into a local `AudioBuffer` (under the
   SpinLock, ~1 ms for 8 s stereo @ 48 kHz).
3. If fewer than 2 s of non-silent audio exist → "Not enough audio has been captured".
4. `TrackAnalyzer::analyze()` (pure function, unit-tested) computes RMS, peak,
   approximate loudness, spectral centroid/rolloff, six band energies, chroma-based
   key + mode + confidence (Krumhansl–Schmuckler profiles), stereo width (mid/side),
   onset density, and a tempo estimate used only when the host gives no BPM.
5. Result stored under a mutex; `sendChangeMessage()` updates the UI.

Host BPM always wins over the estimate. If confidence is too low, the key is
reported as **Unknown** rather than a guess.

---

## 7. Generation flow

```
UI: Generate ──► GenerationController::enqueue(request)  (message thread; rejects if unlicensed / queue full)
                     │
                     ▼ generation worker
     provider.submit ─► poll (backoff, timeout) ─► provider.download → temp file
                     ─► validate (size ≤ 100 MB, real decoder opens it, length > 0)
                     ─► atomic rename into managed folder with a sanitized, unique name
                     ─► HistoryStore.add ─► CacheManager.enforceLimit (file worker)
                     ─► state = Complete, sendChangeMessage
```

Job states: `Idle, Queued, Submitting, Generating, Polling, Downloading, Processing,
Complete, Cancelled, Failed`.

* One active job + up to 3 queued. The UI disables Generate while a job runs, so
  rapid clicking cannot cause a request storm.
* Every network/provider call receives a `CancellationToken`. Cancel, plugin
  deletion and DAW shutdown all trip it.
* Retry policy: connection-level failures and 5xx during **polling/download** retry
  with exponential backoff (max 5). **Submit** is retried only when the request was
  never delivered (connection refused). 4xx (except 429) are never retried.
* Errors are mapped to `ProviderError { code, userMessage, technicalDetail }`.
  The UI shows the user message; technical detail is behind a "Details" toggle.

---

## 8. Provider abstraction

```
IAudioGenerationProvider            (called only from the generation worker)
 ├─ id(), displayName()
 ├─ testConnection(token)  → ConnectionTestResult
 ├─ getCapabilities()      → Capabilities (types + features)
 ├─ submit(request, token) → SubmitResult { jobId }
 ├─ poll(jobId, token)     → PollResult { state, progress, audio location }
 ├─ cancel(jobId, token)
 └─ download(poll, destTempFile, token) → DownloadResult

MockAudioGenerationProvider     fully offline; synthesizes kicks/snares/hats/bass/pads/
                                vocal-like formants/FX/loops; simulates latency, progress,
                                polling, cancellation and (optional) failures
LocalGenerationBridgeProvider   HTTP/JSON via BridgeApiAdapter + BridgeConfiguration
(future) OfficialProvider / LocalModelProvider — same interface
```

Capabilities drive the UI: unsupported generation types, Variation, and
"Send Audio Context" are disabled with a tooltip explaining why.

### Bridge configuration

`BridgeConfiguration` is **data only** (no scripts, no code). It is loaded from
the built-in profile or `%APPDATA%\Context\bridge_profiles.json`, and validated:
paths must start with `/`, contain no `..`, scheme or host; JSON field paths may
only contain `[A-Za-z0-9_-]` segments separated by `.`; status mappings may only
target known job states; timeouts are clamped.

`BridgeApiAdapter` is the only code that knows endpoint paths and JSON field
names: it serializes requests, extracts job ID / status / progress / audio URL /
error, and classifies HTTP failures.

The built-in profile implements the **Context Bridge Protocol v1**
(`docs/BRIDGE_PROTOCOL.md`), a contract defined by this project. No third-party
bridge endpoints are assumed. A bridge with a different API is connected by
writing a profile, not by changing code.

---

## 9. Licensing

* Backend: FastAPI + SQLite, Ed25519 private key on the server only.
* Activation: plugin sends license key, hashed device ID, plugin version →
  server returns a signed token `base64url(payload).base64url(signature)`.
* The plugin embeds only the **public** key (from `licensing/license_public_key.txt`,
  compiled in by CMake) and verifies signatures with Monocypher.
* Token payload carries `device_id`, `refresh_after` (24 h) and `offline_until`
  (7 days). The plugin is licensed while the signature is valid, the device
  matches and `now < offline_until`. After `refresh_after` it revalidates in the
  background; network failure keeps the current token (grace), an explicit
  server refusal (revoked/expired/unknown) clears it.
* License keys are sent only over HTTPS (plain http is allowed to 127.0.0.1 for testing).
* Licensing requests are cancellable, so a dead server never delays DAW shutdown.
* Licensing never touches the audio path. Unlicensed = generation disabled;
  passthrough, analysis and the UI keep working.

Device ID: `SHA-256("context-device-v1:" + OS machine identifier)`, truncated to
32 hex chars. On Windows the identifier is the `MachineGuid` registry value;
elsewhere JUCE's `SystemStats::getUniqueDeviceID()`. No MAC addresses, serials or
user names are collected.

---

## 10. File lifecycle and cache

```
%APPDATA%\Context\
  settings.json         atomic writes (temp + replace); corrupt → backed up, defaults used
  license.json          token + license key (local only)
  history.json          atomic writes; corrupt → renamed *.corrupt-<time>.json, fresh start
  bridge_profiles.json  optional user profiles (data only)
  Generated\            managed audio folder (configurable)
  Temp\                 downloads in progress / audio-context snapshots, cleaned on start
  Logs\context.log      rotated at 2 MB, 3 files kept, secrets redacted
```

Downloads: `Temp\<id>.part` → validate → rename to
`Generated\Context_142BPM_FSharpMinor_Vocal_2026-10-16_001.wav`.

Cache cleanup deletes a file only if **all** are true:
1. it is tracked in history,
2. it is not a favorite and not *pinned* — a file becomes pinned when it is dragged into a
   DAW, its path is copied, it is shown in Explorer or exported, because DAWs such as
   FL Studio reference dragged files in place and deleting them would break projects,
3. it is a direct child of the current managed folder (canonical path check),
4. its name matches the `Context_*.wav/.aiff` pattern Context creates.

Least-recently-used entries go first. Exports ("Save As") are copies outside the
managed folder and are never tracked for deletion.

---

## 11. DAW lifecycle

| Event | Handling |
|---|---|
| Plugin scan | Constructor is cheap: no threads doing work, no network, no file scans. Shared services load lazily on first real instance. |
| No editor | Everything works headless; jobs finish and land in history. |
| Editor close/reopen | Editor re-reads controller state; jobs continue untouched. |
| Save/reopen project | `get/setStateInformation` stores only type, prompt, toggles, UI tab. |
| Sample-rate / block-size change | `prepareToPlay` reallocates the capture ring; preview resamples on the fly. |
| Offline render | Passthrough + capture only; preview muted. |
| Plugin deleted during generation | Processor destructor cancels tokens (aborting HTTP), joins workers, then destroys members. |
| Multiple instances | Per-instance controllers; process-wide shared stores with internal locking. |
| DAW shutdown | Same as deletion; last shared-service owner flushes history/settings. |

---

## 12. GitHub CI

`.github/workflows/build-windows-vst3.yml` on `windows-latest`:
checkout → Python → pinned JUCE via CMake FetchContent (cached) → configure (default
Visual Studio generator, x64) → `cmake --build --config Release` → backend pytest →
`ctest -C Release` (unit tests, bridge integration against the reference bridge, licensing
integration against the real FastAPI backend, and `vst3_host_load`, which loads the freshly
built `Context.vst3` through JUCE's VST3 hosting exactly like a DAW) → find `Context.vst3`
bundle (fail if missing) → artifact `Context-Windows-VST3`. Tags `v*` additionally create a GitHub
Release with `Context-Windows-VST3-<tag>.zip`. No secrets are needed to build.

---

## 13. Future macOS plan

* No Windows-only APIs outside `#if JUCE_WINDOWS` blocks (device ID only).
* Add `AU` to `FORMATS` in CMake, a `macos-latest` job, codesigning + notarization.
* Paths come from `juce::File::getSpecialLocation`, so `~/Library/Application Support/Context` works unchanged.

---

## 14. Prompt building

`PromptBuilder` (pure function, unit-tested) turns prompt + type + analysis into an expanded
prompt plus structured fields (bpm, key, mode). Rules: an explicit BPM/key in the prompt is
never overridden; drums get no key unless tuned (808, toms, cowbell); low-end reduction is
suggested only when the track is bass-heavy *and* the user did not ask for bass; stereo,
density and tone hints depend on the generation type. With expansion off, the prompt is sent
as typed but structured fields still go along.

## 15. Architecture review (done before implementation)

| Concern | Finding / fix |
|---|---|
| Realtime | Capture uses tryLock; preview clips decoded off-thread and handed over through a lock-free FIFO; retired clips freed on the message thread. |
| Races | Controller state guarded by leaf mutexes; UI copies state under lock, then renders. |
| Shutdown | Destructors cancel → join → destroy; tokens abort in-flight HTTP. |
| Security | Bridge restricted to loopback by default; non-local requires opt-in + acknowledgement; downloads size-capped and decoder-validated; profiles are data-only; logs redacted; license backend never sees prompts or audio. |
| JSON | Responses size-capped (2 MB) and nesting-depth checked before parsing to avoid stack exhaustion. |
