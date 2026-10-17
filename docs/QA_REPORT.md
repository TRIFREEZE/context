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
