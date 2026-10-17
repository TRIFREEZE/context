# Context

**Generate the sound you need without leaving your DAW.**

Context is a VST3 plugin for music producers. Put it on a track, let it listen,
type what you want ("dark distorted female vocal chop"), and it generates a sound
that fits your track's tempo, key and mix — then you drag the WAV straight into
your DAW.

* Windows 11 x64, VST3. Built for FL Studio; designed to work in Ableton Live,
  REAPER, Studio One and Cubase.
* Generation runs through a **generation bridge on your own computer**
  (`http://127.0.0.1:3000`). Context never handles provider passwords, cookies or tokens.
* A built-in **Mock Provider** makes the whole product testable with no AI service at all.
* Context's own licensing (license keys, 2 devices, 7-day offline grace) runs on a
  small FastAPI server that you host.

### New in V2 — the workspace

* **TRACK CONTEXT lane** — capture exactly **8 bars** of your song, starting on a bar line
  from the DAW's tempo and position; tempo and key show in the lane header.
* **AI / SAMPLE lane** — the generated sound (or any WAV/AIFF/FLAC/MP3 you drop in) sits
  under your track on the same bar ruler; both lanes play in sync (Both / Context / Output,
  mute, solo, level, loop).
* **BARS 2 / 4 / 6 / 8 / 16** — sets the generation length and fits the result to exactly
  that many bars (sample-exact) with *Auto / Fit / Trim / Loop / Repitch* and five stretch
  algorithms (Percussive, Monophonic, Polyphonic, Texture, Repitch).
* **PROCESSING rack** — 20 built-in effects, reorderable, bypassable, with an inspector,
  click-free parameter smoothing and Processed / Original A/B. Non-destructive.
* **Drag to DAW** — drags exactly what you hear (fit + effects) as a 24-bit WAV, N bars long.
* Everything is saved with the DAW project (settings and references only — no audio, no
  credentials). Older projects load unchanged.

### New in V2.1.1 (Send my track fix)

* **Send my track works.** Two bugs stopped it: the plugin encoded your track in JUCE's private
  base64 variant, which the bridge rejected; and with animations on, the confirm button in every
  dialog (including "Allow and turn on") never ran its action, so the switch stayed off.
* If your track can't be sent, the request stops and tells you why (old bridge, nothing heard yet)
  instead of generating without it. Context re-checks the bridge on its own (when you turn Send my
  track on, before sending, and every 10 s while the bridge is offline), so a bridge started or
  updated after the DAW is picked up without clicking Test Connection.
* While sending, the status line says what goes out, e.g. "Sending your 8 bars (Layer)...".

### New in V2.1

* **Send my track** — the captured bars go to the bridge so results are made *on* your track:
  *Layer* (a vocal or instrument part that plays on top, sample-aligned), *Restyle* or *Continue*.
  The Kie.ai bridge does this with Kie.ai's documented upload, add-vocals, cover, extend and
  stem-separation models.
* **No dead space** — silence before the first sound (and dead air at the end) is removed from
  generations, so they start on bar 1.
* **Edit like a sampler** — select, split, delete, move, trim and stretch clips on the sound lane;
  comp bars from earlier **takes**; **fill a selection with a new take** in the same style.
* **Undo / redo** for everything that is the work (edits, takes, fit, effects, knob drags, captures).
* **Keyboard** — Space, Home, L, 1/2/3, R, A, S, F, Delete, Ctrl+C/X/V/D/A, Ctrl+Z, `?` for the list.
* **Interface** — Barlow type, graphite surfaces, motion on every control (Settings → Reduce
  motion turns it off), and one consistent dialog style everywhere.

See [ARCHITECTURE.md — Parts II and III](ARCHITECTURE.md#part-ii--context-v2-workspace) and
[docs/OPERATING_GUIDE.md](docs/OPERATING_GUIDE.md) sections R–U2.

---

## Contents

1. [Download a ready-built VST3 from GitHub Actions](#downloading-context-from-github-actions)
2. [Install in FL Studio](#install-in-fl-studio)
3. [Quick start: test everything with the Mock Provider](#quick-start-mock-provider)
4. [Licensing backend](#licensing-backend)
5. [Connecting a local generation bridge](#connecting-a-local-generation-bridge)
6. [Building it yourself](#building-it-yourself)
7. [Running the tests](#running-the-tests)
8. [Repository structure](#repository-structure)
9. [Common errors](#common-errors)
10. [Diagnostic logs](#diagnostic-logs)

See also: [ARCHITECTURE.md](ARCHITECTURE.md) · [SECURITY.md](SECURITY.md) ·
[docs/OPERATING_GUIDE.md](docs/OPERATING_GUIDE.md) (step-by-step for non-programmers) ·
[docs/BRIDGE_PROTOCOL.md](docs/BRIDGE_PROTOCOL.md) · [docs/BRIDGE_PROFILES.md](docs/BRIDGE_PROFILES.md) ·
[docs/QA_REPORT.md](docs/QA_REPORT.md) · [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) · [LICENSE_NOTES.md](LICENSE_NOTES.md)

---

## DOWNLOADING CONTEXT FROM GITHUB ACTIONS

Every push to `main` builds Context on GitHub's Windows machines. No compiler needed on your PC.

1. Open the GitHub repository.
2. Click **Actions**.
3. Open the newest successful (green check) **Windows VST3** workflow run.
4. Scroll to **Artifacts**.
5. Click **Context-Windows-VST3**.
6. Download the ZIP.
7. Extract it.
8. Find **Context.vst3** (it's a folder — that's normal for VST3).
9. Copy it to the normal Windows VST3 plugin directory: `C:\Program Files\Common Files\VST3`
10. Open FL Studio.
11. Run a plugin scan.
12. Load Context.

Tagged releases (`v0.1.0`, ...) also appear under **Releases** as
`Context-Windows-VST3-v0.1.0.zip`.

> Artifacts are kept for 30 days. A **Context-Windows-Standalone** artifact is also
> produced: a standalone app useful for debugging outside a DAW.

## Install in FL Studio

1. Close FL Studio.
2. Copy the **whole `Context.vst3` folder** into `C:\Program Files\Common Files\VST3`.
   Windows will ask for administrator permission. If an older `Context.vst3` is there,
   replace only that folder — don't touch other plugins.
3. Open FL Studio → **Options → Manage plugins** → click **Find installed plugins**
   (tick **Verify plugins** for a thorough rescan).
4. Find **Context** in the list (category Effects) and click the star to add it to your favorites.
5. Open the **Mixer**, pick the insert of the track (or the Master) you want Context to
   listen to, click an empty slot → **Context**.

Context is an *effect*: it passes audio through unchanged and listens to it. Previews
play through that same mixer insert.

Other DAWs: rescan VST3 plugins (Ableton: *Preferences → Plug-Ins → Rescan*; REAPER:
*Options → Preferences → VST → Re-scan*; Studio One: *Options → Locations → VST Plug-Ins*;
Cubase: *Studio → VST Plug-in Manager*).

## Quick start: Mock Provider

The Mock Provider synthesises test sounds locally (kicks, snares, hats, 808s,
cowbells, pads, formant "vocals", plucks, risers, melodies, loops) so you can test the
full workflow with no AI service.

1. Load Context. The first-run screen asks for a license key: start the licensing
   backend and create a test license first ([next section](#licensing-backend)), or click
   **Skip for now** to look around (generation stays disabled until activated).
2. On step 2, click **Use Mock Provider (offline test)**.
3. Play your track for a few seconds, click **Analyze Track**.
4. Type a prompt, pick a type, click **GENERATE**.
5. Click the waveform or **Play** to preview, then **drag the waveform into FL Studio**.

Test triggers you can add to any prompt with the Mock Provider: `#fail` (simulated
failure), `#slow` (slower generation), `#timeout` (never finishes; tests the timeout).

## Licensing backend

The backend only handles Context licenses and version info. It never sees prompts or audio.

**Windows, simplest way:**

1. Install Python 3.10+ from python.org (tick *Add Python to PATH*).
2. Put the private signing key at `backend\keys\signing_key.pem`
   (the one created for this project — see [Signing keys](#signing-keys)).
3. Double-click `scripts\run-license-server.bat` → server runs at `http://127.0.0.1:8787`.
4. In a second window, double-click `scripts\create-test-license.bat` → it prints a key
   like `CTX-7K2M-9QXR-4HTP-WB3N`.
5. In Context: paste the key → **Activate**.

**Admin commands** (from the `backend` folder, using `.venv\Scripts\python` on Windows):

```
python -m app.admin create --max-devices 2 [--expires-days 365] [--note "customer email"]
python -m app.admin list
python -m app.admin activations [KEY]
python -m app.admin revoke KEY
python -m app.admin restore KEY
python -m app.admin reset KEY          # free all device slots
```

**Configuration** (environment variables): `CONTEXT_LICENSE_DB`,
`CONTEXT_SIGNING_KEY_FILE`, `CONTEXT_OFFLINE_GRACE_DAYS` (7), `CONTEXT_REFRESH_HOURS` (24),
`CONTEXT_DEFAULT_MAX_DEVICES` (2), `CONTEXT_RATE_LIMIT_PER_MINUTE` (30),
`CONTEXT_LATEST_VERSION`, `CONTEXT_DOWNLOAD_URL`, `CONTEXT_RELEASE_NOTES`.

The plugin's licensing server address is in **Settings → License → Licensing server**
(default `http://127.0.0.1:8787`; change the default in `CMakeLists.txt`,
`CONTEXT_LICENSE_SERVER_URL`). Non-local servers must use `https://`.

### Signing keys

* `licensing/license_public_key.txt` — **public** key, committed, compiled into the plugin.
* `backend/keys/signing_key.pem` — **private** key, git-ignored. Never commit it, never
  put it in GitHub Actions, never send it to anyone.

The two must match. To start over with a brand-new pair:
`python -m app.admin init-keys --force` (writes both files), then commit
`licensing/license_public_key.txt` and rebuild. Existing activations stop validating.

## Connecting a local generation bridge

Context talks HTTP/JSON to a bridge you run on your computer. The bridge does the
actual generation and its own provider sign-in.

1. Start your bridge (it should listen on `127.0.0.1`).
2. Context → **Settings → Generation**: Provider **Local Generation Bridge**,
   Bridge address `http://127.0.0.1:3000` (or your port) → **Test Connection**.
3. Status shows **✓ Local generation bridge detected** — or what's wrong
   (Connection Refused, Unsupported API, Generation Endpoint Missing, ...).

The built-in profile speaks the [Context Bridge Protocol v1](docs/BRIDGE_PROTOCOL.md).
For a bridge with a different API, write a [bridge profile](docs/BRIDGE_PROFILES.md)
(configuration only). **No third-party bridge API is assumed**: map only endpoints
your bridge documents.

To try the bridge path without any AI service, run the reference bridge:
`scripts\run-reference-bridge.bat` (or `python tools/reference_bridge/reference_bridge.py --port 3000`).

For real AI audio, run the **Kie.ai bridge**: `scripts\run-kie-bridge.bat` (your own Kie.ai API key,
stored only in your Windows profile). See [docs/KIE_BRIDGE.md](docs/KIE_BRIDGE.md) — including why
Kie.ai (a third-party Suno reseller) is fine for prototyping but uncertain for commercial use.

Non-local bridges (another PC, LAN) are refused unless you tick **Allow non-local
generation bridge** and confirm the warning.

## Building it yourself

GitHub Actions is the easy path. To build locally on Windows you need Visual Studio
2022 or newer (workload *Desktop development with C++*), CMake 3.22+, Git and Python.

```
scripts\build-windows.bat
```
Output: `build\Context_artefacts\Release\VST3\Context.vst3`.
Add `-Install` (from an Administrator PowerShell) to copy it into `C:\Program Files\Common Files\VST3`.

Linux (development only): `scripts/build-linux.sh`.

JUCE is pinned to **8.0.12** (exact commit in `dependencies/JUCE_COMMIT`); scripts
fetch exactly that commit.

### Renaming the product

Change `CONTEXT_PRODUCT_NAME` (and the codes) at the top of `CMakeLists.txt` and
`PRODUCT_NAME` at the top of `.github/workflows/build-windows-vst3.yml`. The name
drives the plugin name, bundle name, app-data folder and generated file names.
Changing it creates a "new" plugin as far as DAWs are concerned.

## Running the tests

```
ctest --test-dir build -C Release --output-on-failure
python -m pytest backend/tests
```

| Test | What it covers |
|---|---|
| `unit_tests` | ~5,700 checks. V2.1: arrangement operations and rendering (bit-exact splits, crossfaded fills, stretch), takes and fills, undo/redo incl. knob gestures, dead-space removal, context-aligned fitting, keyboard shortcuts, context-mode serialisation. V2: bar/sample maths (120 BPM 4/4: 4 bars = 8 s, 8 bars = 16 s, several sample rates), exact-bar capture termination, resampling, every fit mode and stretch algorithm, effect parameters/serialisation, chain reorder/bypass/latency, "every visible control changes the audio", two-lane sync, session state (old, missing and malformed fields). Original: passthrough at 44.1/48/96 kHz and 16–4096-sample blocks, capture ring buffer (incl. concurrency), key/spectral/stereo/transient analysis, PromptBuilder, filename sanitiser, redaction, URL policy, JSON hardening, bridge profiles and adapter parsing, mock generation (cancel, timeout, failures, rapid clicks), history/cache rules, license tokens and offline grace, processor state, editor open/close, UI snapshots |
| `bridge_integration` | real HTTP against the reference bridge: connection statuses, generation, audio-context upload, Unicode prompts, cancel, and every failure mode (malformed/huge/deep JSON, bad/empty/150 MB WAV, expired URL, vanished job, bridge dying mid-request, non-local audio URL) |
| `license_integration` | real FastAPI backend + the plugin's C++ license manager: activation, reload, device limit, deactivation, revocation, wrong public key, update check |
| `vst3_host_load` | loads the **built Context.vst3** through JUCE's VST3 hosting like a DAW: scan, instantiate at 3 sample rates, bit-exact passthrough, state, editor, two instances |
| `backend/tests` | key generation, activation, device limits, revocation, expiry, tampered tokens, input validation, SQL-injection attempts, rate limiting, admin CLI |

## Repository structure

```
CMakeLists.txt              product identity, pinned JUCE, targets
dependencies/               JUCE_COMMIT / JUCE_VERSION (the pin)
licensing/                  license_public_key.txt (public; compiled in)
plugin/Source/
  PluginProcessor.*         audio entry point (passthrough, capture, preview, V2 workspace engine)
  PluginEditor.*            window, header, tabs
  Analysis/                 TrackAnalyzer (key, bands, stereo, transients, tempo) + worker
  Audio/                    CaptureBuffer (rolling buffer), PreviewPlayer; V2: MusicalTime,
                            AudioAsset, Resampler, ContextRecorder, PreviewEngine, TimeFit,
                            TimeStretch/ (phase vocoder, WSOLA, transient slicing, repitch)
  DSP/                      V2 effect system: parameters, processor base, registry, chain,
                            Effects/ (20 built-in effects)
  Session/                  V2 session state, controller, context cache
  Core/                     settings, shared services, paths, worker thread, build info
  Generation/               provider interface, Mock + Local Bridge providers, bridge
                            profiles/adapter, PromptBuilder, synth, job controller
  Licensing/                device ID, Ed25519 token verification, license manager
  Networking/               HTTP client, audio file validator
  Storage/                  logger, history, cache manager
  UI/                       design system (Theme, Controls), Workspace (prompt, lanes, fit,
                            effect rack, transport), History / Settings tabs, onboarding
plugin/Resources/fonts/     Barlow (SIL OFL 1.1), embedded in the plugin
  Utils/                    sanitiser, redaction, URL safety, JSON safety, atomic files
backend/                    FastAPI licensing server, admin CLI, tests
tools/reference_bridge/     test bridge implementing the Context Bridge Protocol
tools/kie_bridge/           Kie.ai Suno API bridge (real generation) + fake Kie.ai server for tests
tests/                      C++ unit tests, integration drivers, VST3 host test
scripts/                    build / server / bridge helper scripts
docs/                       protocol, profiles, operating guide, QA report
third_party/monocypher/     Ed25519 verification (BSD-2/CC0)
.github/workflows/          Windows VST3 build + release
```

## Common errors

| Symptom | Fix |
|---|---|
| Context doesn't appear in FL Studio | Copy the whole `Context.vst3` **folder** to `C:\Program Files\Common Files\VST3`, then *Find installed plugins* with *Verify plugins*. |
| "Couldn't reach the Context licensing server" | Start `run-license-server.bat`; check Settings → License → Licensing server is `http://127.0.0.1:8787`. |
| "The licensing server's response could not be verified ... different signing keys" | The plugin's public key doesn't match `backend\keys\signing_key.pem`. Use the matching key, or run `init-keys --force`, commit the public key and rebuild. |
| "This license is already active on 2 device(s)" | Deactivate on another computer (Settings → Deactivate) or `python -m app.admin reset KEY`. |
| Bridge: Offline / No bridge detected | Start your bridge; check the port; click Test Connection. A system proxy or firewall can block `127.0.0.1` on rare setups. |
| Unsupported API | The bridge doesn't speak the selected profile's protocol: choose/write the right [bridge profile](docs/BRIDGE_PROFILES.md). |
| "Your local bridge reports its upstream sign-in has expired" | Sign in again *in your bridge*. Context never handles that login. |
| Preview is silent | The host must be processing audio for the track Context is on (FL Studio: make sure the mixer track isn't disabled). Dragging works regardless. |
| Typing in the prompt doesn't work in FL Studio | Click into the prompt box first. If FL Studio still captures the keys, look in the plugin wrapper's settings (the ⚙ menu on the plugin window) for its keyboard-focus option. |
| GitHub build failed | Open the failed run, expand the red step, copy the last ~50 lines, paste them into Claude. |

## Diagnostic logs

* Logs: `%APPDATA%\Context\Logs\context.log` (rotated at 2 MB, 3 files kept).
* Settings → Diagnostics → **Copy Diagnostic Info** copies version, build, OS, DAW,
  sample rate, bridge URL, statuses, recent error codes and the last log lines —
  secrets are redacted automatically. Paste that into a support message or into Claude.
* Settings → Diagnostics → Log level → *Verbose (debug)* for more detail.
