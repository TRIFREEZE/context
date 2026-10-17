# QA report (MVP)

Environment used for development and testing: Linux (Ubuntu 24.04, GCC 13, CMake 3.28,
Python 3.13), JUCE 8.0.12. **No Windows machine, no FL Studio and no GitHub access were
available**, so everything below marked *Linux* was executed for real on Linux, and
everything Windows-specific is *unverified* until the first GitHub Actions run and a
manual FL Studio session.

## Automated test results (last run)

| Suite | Result |
|---|---|
| `unit_tests` (C++) | 800+ checks, 0 failures |
| `bridge_integration` (C++ ↔ reference bridge over real HTTP) | 90 checks, 0 failures |
| `license_integration` (C++ ↔ FastAPI backend) | 28 checks, 0 failures |
| `vst3_host_load` (built Context.vst3 loaded via JUCE VST3 hosting) | all checks pass (editor checks run under a virtual display) |
| Product end-to-end with the real compiled-in public key (manual run) | 16 checks, 0 failures |
| `backend/tests` (pytest) | 15 tests pass |
| Workflow YAML (`actionlint`) | no problems |
| Windows-target syntax check of all 46 source files (MinGW headers) | 0 errors in Context code |
| Standalone app launch (virtual display) | starts, connects/logs, no crash |

## Adversarial QA

| Scenario | How it was exercised | Result |
|---|---|---|
| Close DAW / delete plugin during generation | destroy controller mid-job (mock + real HTTP); processor destruction tests | stops in < 3 s, no crash |
| Bridge disappears during request | reference bridge closes the socket mid-poll (`#die`); closed port | retried 5x, then "Local generation bridge is offline." |
| Licensing server offline / internet gone | server URL to a closed port; clock advanced 3 and 8 days | licensed in grace; expires after 7 days with a clear message; audio unaffected |
| Invalid JSON / huge JSON / deep nesting | `#malformed`, `#huge` (5 MB), `#deep` (100 000 brackets) | friendly "unexpected response" error |
| Invalid / empty / 150 MB WAV | `#badwav`, `#emptywav`, `#bigfile` | decode / size errors, no files left behind |
| Expired download URL, vanished job | `#expired` (410), `#vanish` (404) | specific friendly errors |
| Non-local audio URL from bridge | `#remoteurl` | refused |
| Path traversal / injection | job IDs with `../`, URL credentials, profile paths with `..`/`://`, header CR/LF | rejected or escaped |
| Empty, very long, Unicode prompts | validation tests; Unicode + 800-char prompt through real HTTP | handled |
| Strange Windows filename characters, reserved names | sanitiser tests (`< > : " / \ | ? *`, control chars, `CON`, `NUL`, `..`) | sanitised |
| Corrupt history / settings / license / profile files | garbage, truncated, oversized, 70 000-bracket files | moved aside or ignored; defaults used |
| Invalid bridge address | 11 malformed URLs | rejected with a reason |
| Rapid Generate presses | 25 enqueues in a loop | ≤ 4 accepted, all complete, no storm |
| Rapid Analyze presses | 20–50 requests | exactly one analysis runs |
| Editor opened repeatedly / two instances | 5 open/close cycles × 2 instances; host test 3 cycles | no leaks/crash (JUCE leak detector silent) |
| Instantiated without editor | all processor and host tests | full function headless |
| Silence, mono, stereo, 44.1/48/96 kHz, block 16…4096 | processor + host tests | bit-exact passthrough everywhere |
| Host BPM missing / present | fake play-head | handled; host BPM wins over estimate |
| Sample-rate change while running | 48 k → 96 k | capture reallocated safely |
| Offline render with preview playing | `isNonRealtime()` | preview muted, render clean |
| Audio thread never blocks | concurrent push vs snapshot vs reallocation, timing assertion | no push took > 2 ms |
| Garbage DAW state | junk bytes, null, 2 MB block | ignored |

## Self-review findings that were fixed

1. **`juce::var::isObject()` is true for arrays** — a bridge answering `[1,2,3]` would have
   been treated as an object. Strict `json::isObject` used everywhere.
2. **Cache cleanup could break projects**: FL Studio references dragged files in place, so
   deleting an old cached WAV would make a sample go missing in a saved project. Files that
   were dragged, exported, revealed or had their path copied are now *pinned* and never
   auto-deleted.
3. **Pending saves could outlive their store** (found by a test segfault). Stores now wait
   for queued writes before destruction.
4. **Licensing requests could hold up DAW shutdown** for up to the HTTP timeout. They are now
   cancelled on shutdown.
5. **License keys could be sent over plain HTTP** to a remote server. Now https-only except
   localhost.
6. **Integration tests silently ran zero checks** (JUCE long options need `--name=value`).
   Fixed, and the test now fails if no bridge URL is supplied.
7. **Admin CLI default argument was bound at import time**, so a test wrote a throwaway key
   into the real `licensing/` file. Fixed and the stray key removed.
8. "huge sub bass" was being read as a *stereo-width* request; "huge" removed from width intent.
9. A single sustained tone was reported as a (51 %) key; confidence threshold raised so it
   reads **Unknown**.
10. Typed slider values in Settings weren't saved (only drags were).
11. Windows takes ~2 s to refuse a localhost connection; "Connection Refused" vs
    "Disconnected" threshold adjusted.
12. Structured-binding loops over braced tuples rewritten with explicit types (MSVC safety).

## Review notes

**Audio plugin engineer** — processBlock does: denormal guard, play-head read into atomics,
silence check, `SpinLock::tryEnter` + memcpy into a preallocated ring, preview mix from a
pre-decoded clip. No allocation, I/O, logging, locks that wait, or JSON. Preview clips are
swapped through an atomic pointer and freed on the message thread. Workers are owned by
controllers and joined before the members they use are destroyed; UI listeners unregister in
destructors; async dialogs use `SafePointer`.

**Security engineer** — see SECURITY.md. No code reads browser data; no credential fields
exist anywhere in the UI, settings or requests; WinINet runs with `NO_COOKIES`.

**Producer / product** — main path is: type → (Analyze) → Generate → click waveform to hear
→ drag. Enter generates. Errors are one sentence with a *Details* button. Unsupported
types/features are disabled with tooltips instead of failing.

**Maintainer** — one adapter owns all bridge knowledge; providers are swappable; product
name in one place; pinned JUCE with a version guard; Windows-only code limited to the
device-ID registry read.

## Not verified (needs a Windows machine)

* MSVC compilation and the GitHub Actions run itself.
* Loading in FL Studio, Ableton Live, REAPER, Studio One, Cubase.
* Drag-and-drop of WAVs from the plugin window into each DAW (uses JUCE's OLE drag on Windows).
* Preview audibility in each host (depends on the host processing the track).
* Keyboard focus behaviour in FL Studio's plugin wrapper.
* High-DPI scaling on Windows.
* Real third-party bridge connectivity (no third-party bridge API is assumed or included).

---

## V2 workspace — verification (October 2026)

Run on Linux x64 (GCC, Ninja, Release, JUCE 8.0.12) — **not** on Windows:

| Check | Result |
|---|---|
| Full build (plugin VST3 + Standalone + tests), `-Wall -Wextra` | builds, 0 warnings in V2 sources |
| `ctest` — `unit_tests`, `bridge_integration`, `kie_bridge_e2e`, `license_integration`, `vst3_host_load` | 5/5 passed |
| `ContextTests --unit` | 4,751 checks, 0 failures |
| Bar maths: 120 BPM 4/4 → 4 bars = 8.000 s, 8 bars = 16.000 s; sample counts at 44.1/48/88.2/96 kHz | pass |
| Capture: stops at exactly 8 bars, bar-line start inside a block, fails on stop / tempo change / cancel | pass |
| Fitting: every mode × algorithm returns exactly the target length, stereo, finite | pass |
| Effects: all 20 at 44.1/48/96 kHz, odd block sizes; every visible control changes the output | pass |
| Chain: reorder changes the signal, bypass click-free, constant latency, state round trip, junk tolerated | pass |
| Session state: v1 project (no V2 fields), missing fields, malformed JSON, relative paths | pass |
| Processor level (through `processBlock`): 8-bar capture → analyse → cache, import + fit, two-lane playback, export exact-length and reflects effects, host-tempo change flagged + retarget, project save/reopen | pass |
| UI screenshots at 980 / 1120 / 1600 px (empty, armed, loaded, EQ, delay, history, settings) | reviewed |

**Not verified for V2** (needs Windows / FL Studio): everything in the "Not verified" list above,
plus capture against FL Studio's real transport and tempo reporting, MP3 import through
Windows Media components, drag-to-DAW of the processed file into FL Studio's playlist, and
sample-rate changes inside FL Studio. Use the checklist in `docs/OPERATING_GUIDE.md` §U2.


---

## V2.1 — takes, editing, undo, track context, motion (October 2026)

Linux x64 (GCC, Release), same as V2. **Not** run on Windows or in FL Studio.

| Check | Result |
|---|---|
| Full build, `-Wall -Wextra` | 0 warnings in project sources |
| `ctest` (5 suites incl. `kie_bridge_e2e` with the track-context pipeline against the fake Kie.ai) | 5/5 passed |
| `ContextTests --unit` | ~5,700 checks, 0 failures |
| Dead space: leading silence removed to within 4 ms of bar 1, tail dead air removed, imported files and context-aligned results untouched | pass |
| Arrangement: split is bit-exact, delete silences with 2.5 ms fades, fill crossfades, move/trim/stretch keep clips non-overlapping, junk state rejected | pass |
| Takes + fill: a pending fill lands only in its range; other generations become the base take | pass |
| Undo/redo: fills, fit changes, effects, a 0.8 s knob drag as one step | pass |
| Keyboard through the real editor: Space, S, Delete, Ctrl+Z, Ctrl+Shift+Z, L, 1/3 | pass |
| Kie bridge track context: upload, add-vocals + 2-stem separation, cover + 12-stem pick by prompt, restyle, continue with automatic trim, broken WAV refused | pass (fake Kie.ai) |
| Screenshots at 980 / 1120 / 1600 px: empty, armed, loaded, edited, selection, effects, shortcuts sheet, history, settings | reviewed |

Not verified: real Kie.ai responses for the new models (stem file naming, add-vocals length),
animation smoothness on a real Windows display (the vblank clock was only exercised headless),
and FL Studio's handling of plugin keyboard focus.

## V2.1.1 — Send my track (October 2026)

Reported: "Send my track" stays off after confirming the consent dialog; the track never reached Kie.ai.

Root causes found by new tests (both were invisible to the earlier suite):

1. `Sheet::close` (animated path) cleared `shown.onChange` from inside that same callback, destroying the
   running lambda and the captured button action (undefined behaviour: the action silently never ran, or crashed).
   Tests ran with motion disabled, so they took the non-animated path. Fixed with a fire-once flag.
   Affected every confirm sheet with animations on.
2. `BridgeApiAdapter` sent `MemoryBlock::toBase64Encoding()`, which is JUCE's own `<size>.<alphabet>` format,
   not RFC 4648. The bridge's `base64.b64decode(validate=True)` rejected it (`audio_context_rejected`).
   Now `juce::Base64::toBase64`; a unit test pins the exact encoding.

Hardening: a request with Send my track on re-checks stale capabilities, then either sends the track or fails with a
clear reason (never silently drops it); the provider refuses to submit without it; the editor re-tests an offline
bridge every 10 s; the button follows the user's choice for any bridge.

Tests added: `SendTrackTests` (button + consent sheet, motion off and on; worker: stale caps, status line,
bridge without audio, nothing heard), `BridgeE2ETests` "Send my track" (Context's real provider -> kie_bridge ->
fake Kie.ai, layer and restyle; the script asserts the bridge logged and uploaded the track).

Run here (Linux): `ContextTests --unit` 5693 checks, 0 failed; `ctest` 5/5; `run_kie_bridge_e2e.py` passes
with and without the MP3 decoder. Not verified: a real Kie.ai account, Windows/FL Studio by hand.
