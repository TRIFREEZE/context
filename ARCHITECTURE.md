# Context — Architecture

This document describes how Context is put together, who owns what, and which
thread is allowed to do what. It was written before implementation (Phase 2)
and kept in sync with the code afterwards.

* **Part I** (sections 1–15) — the original plugin: licensing, bridge, generation,
  analysis, history. All of it is still in place and unchanged in behaviour.
* **Part II** (sections V2.1–V2.14) — the Context V2 workspace added on top: captured
  track context, the two-lane timeline, musical fitting / time-stretching, the effect
  rack, export and drag-to-DAW. Includes **HOW TO ADD A NEW EFFECT** (V2.7).

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
                         V2: MusicalTime, AudioAsset, Resampler, ContextRecorder, PreviewEngine,
                         TimeFit, TimeStretch/ (stretch engines)
  DSP/                   V2: EffectParameter, EffectProcessor, EffectRegistry, EffectChain,
                         Effects/ (the 20 built-in effects)
  Session/               V2: SessionState, SessionController, ContextCache
  Analysis/              TrackAnalyzer (pure DSP functions), AnalysisController (worker)
  Generation/            request/result models, IAudioGenerationProvider, Mock + LocalBridge providers,
                         BridgeConfiguration/BridgeApiAdapter, PromptBuilder, SynthAudio, GenerationController
  Networking/            HttpClient (cancellable, size-limited), Downloader (temp file → validate → rename)
  Licensing/             DeviceId, LicenseToken (Ed25519 verify), LicenseManager
  Storage/               Logger, HistoryStore, CacheManager
  Security/              compiled-in public key handling
  UI/                    design system (Theme, Controls) and all components; V2 Workspace
  Resources/fonts        Inter (SIL OFL), embedded via juce_add_binary_data
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

---
---

# Part II — Context V2 workspace

V2 was added **next to** the existing system, not in place of it. The generation
controller, bridge providers, licensing, history, analysis and the old preview player
are the same classes as before; V2 consumes their results. If every V2 class were
removed, Part I would still compile and behave as it did.

## V2.1 Existing architecture (what V2 hooks into)

| Existing piece | How V2 uses it |
|---|---|
| `ContextAudioProcessor::processBlock` | Still passes audio through and feeds `CaptureBuffer`. Then, in order: `ContextRecorder::process` (exact-bar capture) and `PreviewEngine::renderAdd` (two-lane audition). Neither allocates, locks or does I/O. |
| `GenerationController` | When a job completes the processor loads the result into the session (`lastAutoLoadedResult` guards against loading twice). The requested duration is now derived from the BARS selector (`makeRequest`, non-OneShot/FX types, capped by the bridge's `maxDurationSeconds`). |
| `AnalysisController` | The captured context is analysed with the existing `TrackAnalyzer`; `setExternalResult` lets the session hand in that result so prompt building uses the captured 8 bars. |
| `HistoryStore` | The output lane references generated results by history id; History → *Load* sends a result to the workspace. |
| Plugin state (`getStateInformation`) | The existing tree gains `version=2`, `session` (JSON) and `chain` (JSON). Everything old is written exactly as before. |
| Editor | `GenerateTab` / `WaveformView` were replaced by `ui::Workspace`; tab ids, History and Settings are unchanged. |

```
ContextAudioProcessor
 ├── (Part I members, unchanged)
 ├── ContextRecorder      exact-bar capture, audio-thread state machine
 ├── PreviewEngine        two-lane, sample-counted audition engine
 ├── fx::EffectChain      the processing rack (also used offline by the exporter)
 └── SessionController    message-thread owner of the workspace + 3 worker threads
                          (loader, renderer, exporter)
```

## V2.2 AudioAsset pipeline

Every piece of audio in the workspace — generated, imported, or captured — becomes an
`AudioAsset` (`Audio/AudioAsset.h`): an **immutable** decoded buffer plus sample rate,
origin (`Generated`, `Imported`, `Context`), source file, name and optional tempo hint,
shared as `std::shared_ptr<const AudioAsset>`. Nothing ever mutates an asset; every
processing step produces a new buffer, which is why "Restore original", A/B and
"Reset fit" are free.

```
file / generation result / capture
        │  assets::loadFile (worker)  — WAV, AIFF, FLAC, OGG; MP3 via the OS decoder (Windows/macOS)
        ▼
   AudioAsset (immutable, source rate, 1–2 ch)
        │  TimeFit::render (worker)   — length in bars × tempo → exact target sample count
        ▼
   fitted stereo buffer at the HOST rate (exactly targetSamples long)
        │  PreviewEngine program      — what you audition (rack applied in real time)
        │  SessionController export   — rack applied offline → 24-bit WAV → drag-to-DAW
```

Generated and imported audio follow the identical path; only `origin` and the tempo hint
differ. Sample-rate conversion uses `Resampler` (Kaiser-windowed sinc, 32 zero crossings,
cut-off lowered when downsampling), so a 44.1 kHz asset in a 48 kHz project is converted
once, off the audio thread.

## V2.3 Context capture

`ContextRecorder` records **exactly 8 bars** (`musical::contextBars`) of the plugin input.

* Length: `musical::samplesForBars (bars, tempo, sampleRate)` — one `llround` of
  `bars × quarterNotesPerBar × 60 / bpm × rate`, where `quarterNotesPerBar = num × 4 / den`.
  120 BPM 4/4: 4 bars = 8 s, 8 bars = 16 s (unit-tested at several rates).
* Start (decided on the audio thread):
  * **HostSync** — host is playing and reports tempo + PPQ: recording starts on the next
    bar line, sample-accurately inside the block.
  * **HostTempo** — tempo but no usable position: starts on the first block above −60 dBFS.
  * **Manual** — no host tempo: uses the manual tempo, same signal trigger.
* Termination: the recorder writes until the target count and switches to `Complete`
  by itself; the remainder of that block is ignored. It fails cleanly (and says why) on
  transport stop, tempo change, buffer overflow or cancel.
* State is one atomic (`Idle → Armed → Recording → Complete/Failed`, plus `Cancelling`)
  changed with compare-and-swap; the buffer is allocated by `arm()` on the message thread.
* The message thread polls the state (session timer), takes the result, and a worker
  analyses it and stores it through `ContextCache` (`context-<id>.wav` + `.json` metadata
  in `%APPDATA%\Context\ContextCache`). The project only stores the id and metadata.

## V2.4 Playback synchronisation

`PreviewEngine` plays both lanes **inside the plugin**, on one sample-counted timeline
(independent of the DAW transport, so it works while the DAW is stopped):

* The message thread publishes a *program* — `{context buffer, output buffer}` at the
  host rate — through an atomic pointer; old programs are retired through a lock-free
  FIFO and freed on the message thread.
* Timeline length = the longer lane; the shorter lane repeats (4 bars under 8 bars
  plays twice). Positions are integers; chunks never cross the timeline end, so the
  lanes cannot drift relative to each other, ever.
* Modes Both / Context / Output, per-lane gain, mute, solo, loop. Start/stop and
  program swaps are crossfaded; Original/Processed A/B is a short crossfade, not a cut.
* The rack runs only on the output lane. Its latency is compensated by reading the
  output lane *ahead* by the chain latency, so the plugin reports 0 latency to the host
  and the lanes stay aligned with any effect combination.
* Nothing is played during offline (bounce) renders or when the program's sample rate
  does not match the host — the audition never leaks into a DAW export.

## V2.5 Time stretching and fitting

`TimeFit` turns an asset into exactly `samplesForBars (lengthBars, sessionTempo, hostRate)`
stereo samples. Fit modes:

| Mode | What it does |
|---|---|
| **Auto** | Picks per material: one-shots → Trim; ≤0.5 % off → Trim; near a whole divisor → grid-locked Loop; 0.75–1.33× → Stretch; much longer → Trim; else Loop. |
| **Fit** (Stretch) | Time-stretch to the target with the selected algorithm, pitch preserved. |
| **Trim** | Cut / pad with a short fade. |
| **Loop** | Repeat; each repeat starts at its own exact bar position (no drift). |
| **Repitch** | Resample (tape-style: tempo and pitch move together). |

Source tempo comes from the generator (requested BPM), then the file name (`..._128bpm`),
then from the length (`bpmForLoop`).

Stretch algorithms (`Audio/TimeStretch/`, all behind `TimeStretchEngine`):

| Algorithm | Engine |
|---|---|
| Percussive | `TransientSliceEngine` — detected onsets are copied verbatim (1.5 ms guard), the gaps WSOLA-stretched, 3 ms slot fades |
| Monophonic | `WsolaEngine` (40 ms frames, coarse-to-fine correlation search) |
| Polyphonic | `PhaseVocoderEngine` (4096/4, identity phase locking, transient phase reset, stereo-coherent phases) |
| Texture | `PhaseVocoderEngine` (8192/8, no transient reset — smooth pads and noise) |
| Repitch | `RepitchEngine` (sinc resampling) |
| Auto | `analyseMaterial` (onset rate, periodicity, spectral flatness) chooses one of the above |

Pitch shift (±24 st) = stretch by the ratio, then resample. Fitting runs on the session's
render worker and reports progress; a newer request makes older results stale (serials).

## V2.6 EffectChain system

```
EffectRegistry::builtIn()  ──create(id)──►  EffectProcessor  (parameters: atomics)
                                                  │
fx::EffectChain                                  ▼
  message thread: ordered model of Slots {instanceId, effect, bypass}  (under a mutex)
        │ every edit publishes an immutable Snapshot (pointer list) via std::atomic
        ▼
  audio thread: reads the latest Snapshot, processes slots in order
        │ replaced snapshots/effects go into a lock-free retire FIFO
        ▼
  message thread: collectGarbage() frees them
```

* Up to 16 effects; add, remove, move (reorder), duplicate, bypass, reset, clear. Wet/dry is
  a `Mix` parameter on the effects where it makes sense (reverb, delay, saturation,
  distortion, compressor, filter, modulation, lo-fi); level effects like EQ and limiter
  don't have one.
* Bypass is a 10 ms crossfade against a latency-aligned dry path, and latency is held
  constant across bypass, so toggling never clicks or shifts timing. A slot woken from
  bypass is reset first (no stale reverb tails).
* Parameters are lock-free atomics written by the UI and smoothed in the DSP
  (`SmoothedParam`), so knob moves never zipper.
* `toVar()` / `fromVar()` serialise `{v:1, fx:[{type, id, bypass, params{...}}]}`; unknown types
  and parameters are skipped, missing ones keep defaults.
* The same chain class renders offline for export (`SessionController::renderProcessed`).

Built-in effects (20): EQ, Filter, Compressor, Limiter, Gate, Transient, Saturation,
Distortion, Soft Clipper, Reverb, Delay, Chorus, Flanger, Phaser, Tremolo, Auto-Pan,
Bitcrusher, Downsampler, Utility, Stereo.

## V2.7 Adding a new DSP effect — HOW TO ADD A NEW EFFECT

1. **Derive from `EffectProcessor`** (`plugin/Source/DSP/EffectProcessor.h`). Put the class in
   `plugin/Source/DSP/Effects/` — a new file pair, or the category file it belongs to.
   ```cpp
   class WidenerEffect : public EffectProcessor
   {
   public:
       WidenerEffect();
       void prepare (double sampleRate, int maxBlockSize) override;
       void reset() noexcept override;
       void process (juce::AudioBuffer<float>&, int numSamples, const ProcessContext&) noexcept override;
   private:
       EffectParameter& width;
       SmoothedParam widthS;
   };
   ```
2. **Define parameters** in the constructor with `addParameter (ParameterSpec::...)`:
   `continuous (id, name, min, max, default, Unit, centre, tooltip)`, `choice (...)`, `onOff (...)`.
   The `id` is saved in projects — never rename it after release. `centre` skews the knob
   (e.g. 1000 Hz for a 20–20k frequency).
   ```cpp
   WidenerEffect::WidenerEffect()
       : EffectProcessor ("widener", "Widener"),
         width (addParameter (ParameterSpec::continuous ("width", "Width", 0.0f, 200.0f, 100.0f,
                                                         Unit::Percent, 0.0f, "Stereo width"))) {}
   ```
3. **Implement `prepare` / `process` / `reset`.**
   * `prepare` (not realtime): store the rate, allocate buffers, call `reset()`.
   * `reset` / `process` (realtime): no allocation, locks, I/O or logging. `process` always
     gets a **stereo** buffer of at most the prepared block size. Read parameters with
     `param.get()` once per block and smooth them.
   * Report `getLatencySamples()` if you delay the signal (the chain compensates),
     `getTailSeconds()` if you ring out, `isParameterRelevant()` to grey out controls that
     currently do nothing, and `getGainReductionDb()` to light up the inspector GR meter.
4. **Register it in `EffectRegistry`**: one line in
   `plugin/Source/DSP/Effects/BuiltInEffects.cpp`:
   ```cpp
   r.registerEffect ({ "widener", "Widener", "WIDE", "Utility", "Mid/side stereo width", make<WidenerEffect>() });
   ```
   The category decides where it appears in the *+ Add effect* menu.
5. **Inspector component** — nothing to write for the standard case: the inspector is
   generated from the parameter list (knobs for continuous, segmented/combo for choices,
   toggles for on/off; double-click resets, typing a value works, tooltips come from the
   spec). For a custom visual (like the EQ curve), add a branch keyed on `typeId()` in
   `EffectRackPanel::rebuildInspector` (`plugin/Source/UI/EffectRack.cpp`), as `EqCurve` does.
6. **Compile**: add any new `.cpp` to `plugin/Sources.cmake` and build. The generic tests in
   `tests/unit/EffectsTests.cpp` iterate the registry, so the new effect is automatically
   checked for: finite, sane output at 44.1/48/96 kHz and odd block sizes; parameter
   clamp / text / state round trips; and "every visible control changes the audio".
   Run `ContextTests --category=V2` (or `--unit`).

Saved state, reordering, bypass, A/B and export pick the effect up
automatically — no chain or UI code changes.

## V2.8 State serialisation

The plugin state tree (`getStateInformation`) keeps every Part I property and adds:

| Property | Content |
|---|---|
| `version` | `2` (projects from 1.x have `1` or nothing) |
| `session` | `SessionState::toVar()` JSON: length in bars, manual tempo/signature, fit mode / algorithm / pitch, lane gains, mute/solo, play mode, loop, A/B, output source (`generatedId` *or* absolute `importedPath`), the replaced generated id (for *Restore generated*), and the context cache id + metadata |
| `chain` | `EffectChain::toVar()` JSON |

* **No audio and no credentials are ever stored in the project** — only ids and paths.
  Context audio lives in the context cache; generated audio in the history folder.
* `SessionState::fromVar` never fails: every field is optional, range-checked and
  defaulted. Old projects (no `session`) load with V2 defaults; malformed JSON, relative
  import paths, unknown enum ids and invalid bar counts are ignored (unit-tested).
* Restore happens on the message thread (`callAsync` + `WeakReference` to the processor),
  because hosts may call `setStateInformation` from any thread.
* Missing files (a deleted sample, a pruned context) show a "missing" lane with the reason
  instead of failing the load.

## V2.9 Offline rendering

* **Fitting** is always offline (worker thread), producing a buffer at the host rate.
* **Export** (`SessionController::renderProcessed`) builds a *fresh* chain from the saved
  chain state, prepares it at the host rate, runs one warm-up pass over the end of the loop
  (so reverb/delay tails wrap into bar 1 exactly as when looping), skips the chain's
  latency, and writes exactly `targetSamples` as a 24-bit WAV. Compare-original exports
  the unprocessed fit.
* The render is staged in the background whenever processing changes; only one staged
  file is kept in `Temp` (cleaned by `CacheManager`). Final exports are content-hashed into
  `Generated\Processed`, so identical sounds are not duplicated.
* The DAW's own offline bounce is untouched: the preview engine is silent when the host
  renders offline.

## V2.10 Drag-to-DAW

`DragToDawPill` (transport bar) and the output lane start
`DragAndDropContainer::performExternalDragDropOfFiles` with the **processed** export
(what you hear: fit + rack, or the original when A/B is on Original). If the background
render is current, the drag starts instantly; otherwise it renders synchronously first
(sub-second for 8 bars). Files dragged into a project live in `Generated\Processed`, which
cache cleanup never touches (it only deletes direct children of `Generated` that belong to
unfavourited history entries). Save / Folder / Copy-path use the same file.

## V2.11 Threading model

| Thread | V2 work | Never |
|---|---|---|
| Audio | `ContextRecorder::process`, `PreviewEngine::renderAdd`, chain `process` on the current snapshot | allocation, locks (other than try-lock), I/O, logging |
| Message | `SessionController` API, UI, chain edits (publish snapshots), garbage collection of retired programs/effects, state save/restore | decoding, fitting, rendering, FFT |
| Session loader | decode files, analyse + cache captured context | touching UI or session state directly |
| Session renderer | TimeFit (stretch/resample) | — |
| Session exporter | offline chain render + WAV write | — |

Results return through `MessageManager::callAsync` guarded by a lifetime token (dropped if
the session was destroyed) and a serial number (dropped if a newer request was made).
Workers are the last members of `SessionController`, so they are joined before anything
they use is destroyed. Audio buffers handed between threads are immutable
(`shared_ptr<const ...>`).

## V2.12 Building locally

Windows (Visual Studio 2022+, *Desktop development with C++*, CMake ≥ 3.22, Git, Python 3.10+
for the integration tests):

```bat
scripts\build-windows.bat
ctest --test-dir build -C Release --output-on-failure
```

The script fetches the pinned JUCE commit (`dependencies/JUCE_COMMIT`, 8.0.12), configures
x64 and builds Release. Linux (development only): `scripts/build-linux.sh`.

The VST3 is at `build\Context_artefacts\Release\VST3\Context.vst3`. Copy it to
`C:\Program Files\Common Files\VST3\` and rescan in FL Studio.

Useful test runs: `ContextTests --unit` (all unit tests), `ContextTests --category=V2`,
`ContextTests --category=Screens` (renders UI screenshots to the temp folder).

## V2.13 GitHub build

Unchanged workflow (`.github/workflows/build-windows-vst3.yml`, see section 12). V2 adds
no secrets and no new external dependencies: the Inter font is compiled in from
`plugin/Resources/fonts` with `juce_add_binary_data`, and all DSP is in-tree. Push and
download the `Context-Windows-VST3` artifact.

## V2.14 Known limitations

* Only Linux builds and tests were run while developing V2; the Windows build is
  done by CI. **FL Studio itself has not been tested** — use the manual QA list in
  `docs/OPERATING_GUIDE.md`.
* MP3 import uses the operating system's decoder (Windows Media Foundation / Core Audio).
  On Windows "N" editions without the Media Feature Pack, MP3 import is unavailable
  (WAV/AIFF/FLAC/OGG always work). No MP3 support on Linux.
* The stretch engines are in-house (no Rubber Band / élastique). Quality is good for
  moderate ratios (0.75–1.33×); extreme ratios sound like what they are.
* The audition engine plays inside the plugin; it is not tied to the DAW transport. Use
  drag-to-DAW to place the sound on the DAW timeline.
* Capturing needs the DAW to play audio *through* Context (put it on the master or a bus).
* No undo stack yet — reset actions exist for effect, chain, fit and source
  ("Restore original/generated"), and the immutable-asset design is ready for a command
  history.
* Effect presets per effect are not implemented; the whole chain is saved with the project.
