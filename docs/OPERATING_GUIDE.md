# Context — Operating Guide

For producers, not programmers. Follow it top to bottom the first time.

You will need: a GitHub account, Git for Windows (https://git-scm.com/download/win),
Python 3.10+ (https://python.org, tick **Add Python to PATH** during install) and FL Studio.

---

## A. Where the finished repository is

You received two things:

* **`Context-repo.zip`** — the whole project (source code, build setup, docs, licensing server).
* **`signing_key.pem`** — the PRIVATE licensing key. Keep it secret. It does **not** go to GitHub.

Unzip `Context-repo.zip` somewhere simple, e.g. `C:\Dev\Context`.

## B. Which files/folders go to GitHub

Everything inside `C:\Dev\Context`, including the hidden `.github` folder.
**Never upload `signing_key.pem` or anything in `backend\keys\`.** The project's
`.gitignore` already blocks keys, build output, WAV files and Python environments —
that's why using Git (below) is safer than dragging files into the GitHub website.

## C + D. Create the GitHub repository

1. github.com → **+** (top right) → **New repository**.
2. Name: `context`. Choose **Private**. Leave "Add a README" **unticked**. **Create repository**.
3. Copy the URL it shows, like `https://github.com/YOUR-NAME/context.git`.

## E. Push the code (exact commands)

Open **Command Prompt** and run:

```
cd C:\Dev\Context
git remote remove origin 2>nul
git remote add origin https://github.com/YOUR-NAME/context.git
git branch -M main
git push -u origin main
```

(The project already has its commit history. If Git asks you to sign in, a browser window opens.)

Later changes:
```
git add -A
git commit -m "describe the change"
git push
```

## F. Where to click in GitHub Actions

1. Your repository page → **Actions** tab.
2. Click the newest run of **Windows VST3** (it starts automatically after the push; ~15–25 min).
3. Green check = success. Red X = failure: click it, click the red step, copy the last ~50
   lines of the log and paste them to Claude.

## G. Where the compiled VST3 appears

Inside the successful run page, scroll to the bottom: **Artifacts**.

## H. What ZIP to download

Click **Context-Windows-VST3** → it downloads `Context-Windows-VST3.zip`. Extract it:
you get a folder **`Context.vst3`** and `INSTALL.txt`.

(Releases: push a tag to get a permanent download:
`git tag v0.1.0` then `git push origin v0.1.0` → **Releases** shows `Context-Windows-VST3-v0.1.0.zip`.
The tag must match the version in `CMakeLists.txt`.)

## I. Where to copy Context.vst3 on Windows

Copy the whole **`Context.vst3` folder** into:

```
C:\Program Files\Common Files\VST3
```

Approve the administrator prompt. Replacing an older `Context.vst3` is fine; don't touch other plugins.

## J. Rescan plugins in FL Studio

1. **Options → Manage plugins**.
2. Click **Find installed plugins** (tick **Verify plugins** for a full rescan).
3. Find **Context** (Effects) and click its star so it shows in your plugin menus.
4. Mixer → choose the insert you want Context to listen to (a track or the Master) → empty slot → **Context**.

## K. Start the licensing backend for testing

1. Put `signing_key.pem` into `C:\Dev\Context\backend\keys\` (create the `keys` folder).
2. Double-click **`scripts\run-license-server.bat`**. First run installs Python packages
   (a minute or two). When you see `Uvicorn running on http://127.0.0.1:8787`, leave the window open.

## L. Create a test Context license

Double-click **`scripts\create-test-license.bat`**. It prints a key like
`CTX-7K2M-9QXR-4HTP-WB3N` (valid on 2 computers). Copy it.

## M. Activate Context

Open Context in FL Studio. The first-run screen says **Step 1 of 2 — Activate Context**:
paste the key → **Activate**. (Later: **Settings → License**.)
Context then works offline for up to 7 days at a time.

## N. Choose the Mock Provider

On **Step 2** click **Use Mock Provider (offline test)**.
(Later: **Settings → Generation → Provider → Mock Provider**.) The header shows a yellow
dot and "Mock Provider (test)".

## O. Test generation without any outside API

With the Mock Provider: type `short punchy trap kick`, choose **One Shot**, click
**GENERATE** (or press Enter). A few seconds later the waveform appears. Try
`hard Memphis cowbell`, `wide ambient synth texture` (Texture), `140 BPM percussion loop` (Loop).
Add `#fail` to a prompt to see how errors look.

## P. Enter the localhost generation bridge address

**Settings → Generation**: Provider **Local Generation Bridge**, Bridge address
`http://127.0.0.1:3000` (or the port your bridge uses). Context never asks for your
provider login — your bridge handles that itself.

## Q. Test the local bridge connection

Start your bridge, then **Test Connection**.
* **✓ Local generation bridge detected** — ready.
* **✕ No bridge detected** — the bridge isn't running or uses another port.
* **Unsupported API / Generation Endpoint Missing** — your bridge speaks a different API: it
  needs a bridge profile (`docs/BRIDGE_PROFILES.md`).

No bridge yet? Double-click `scripts\run-reference-bridge.bat` — a test bridge with simple
tones — and click Test Connection.

## Q2. Real AI sounds with the Kie.ai bridge

The Mock Provider and the reference bridge only make synthetic test tones. For real AI audio:

1. Get a Kie.ai account with credits, and copy your API key from the Kie.ai dashboard.
2. Close the reference bridge if it's running, then double-click **`scripts\run-kie-bridge.bat`**.
   On the first run, paste your API key when asked. Leave the window open.
3. In Context, go to **Settings → Generation → Provider: Local Generation Bridge**, set the address to
   `http://127.0.0.1:3000`, and click **Test Connection** → *Kie.ai Suno Bridge*.
4. Generate as usual. Type **Loop** makes a seamless loop at your BPM. Add **`#song`** to a prompt to
   get a full song.

Details, costs and caveats (Kie.ai is a third-party service, not Suno) are in `docs/KIE_BRIDGE.md`.

## R. The Create screen

```
Prompt ............................................................ [Generate]
Type  One Shot Loop Vocal ...    Bars 2 4 6 8 16          Analysis  Send my track  Enhance
                                 (when Send my track is on:  Use my track to  Layer | Restyle | Continue)
Snap: Beat    1 ---- 2 ---- 3 ---- 4 ---- 5 ---- 6 ---- 7 ---- 8      <- click/drag the ruler to move the playhead
Your track    [captured 8 bars, silver]
Sound  Take 2 of 3   [the sound, teal; clips, takes and edits shown on top]
Fit   Auto Fit Trim Loop Repitch   [algorithm]   Pitch
Processing   [Saturation] -> [Reverb] -> + Add effect                  Processed | Original
|<  >  loop   Both Track Sound   3.2 / 8 bars        New take   save  folder  copy   [Drag to DAW]
```

Hover anything for a tooltip. Right-click a lane for more. Press **?** for every keyboard shortcut.
Undo / redo arrows sit next to the page tabs (Ctrl+Z, Ctrl+Shift+Z).

## S. Capture your track

1. Put Context on the FL Studio **Master** (or a bus everything goes through).
2. Click **Capture** (or press **R**). The lane waits for your DAW.
3. Press play in FL Studio. Context starts on the next bar line, records **exactly 8 bars** (an
   amber bar fills as it records) and stops by itself.
4. Tempo, key and a small low/mid/high readout appear in the lane header. With **Analysis** on,
   they shape the next generation.

No DAW tempo? Click the BPM in the lane header to type one. Changed the song's tempo later? The
header offers **Fit to … BPM** or **Capture again**.

## T. Make a sound

* Type what you want, pick a **Type** and **Bars**, click **Generate** (Enter in the prompt,
  Ctrl+Enter anywhere). The sound lands in the lower lane as a **take**, fitted to exactly that
  many bars at your tempo. Silence before the first sound (and dead air at the end) is removed
  first, so it starts on bar 1.
* **New take** (bottom bar) generates again with the same prompt and settings. Every take stays
  in the **Take N of M** menu, so you can switch back or combine them.
* Or drop a WAV / AIFF / FLAC / MP3 onto the lower lane (or ⋯ → *Load sample*). Your own files are
  never trimmed: Context trusts where they start.
* **Fit**: *Auto* chooses for you; *Fit* time-stretches; *Trim* cuts; *Loop* repeats; *Repitch*
  speeds up or slows down like tape. The sentence on the right says what was done.

### Make it on your track: Send my track

Turn on **Send my track** next to *Analysis*. The first time, Context asks permission (your bars
go to your bridge, and the Kie.ai bridge uploads them to Kie.ai's temporary storage for 24 hours).
Then choose what the generator does with them:

* **Layer**: a part that plays on top of your 8 bars. With *Vocal* you get only a vocal, in time
  and in key. With other types you get the instrument your prompt names (e.g. "dark piano
  melody" gives the keys part). Results sit exactly on your bars: nothing is stretched or moved.
* **Restyle**: your 8 bars re-imagined in the prompt's style.
* **Continue**: what comes after your 8 bars.

These use several Kie.ai steps each, so they cost more credits and take longer than a plain
sound (see `docs/KIE_BRIDGE.md`).

## T2. Edit the sound (cut, move, stretch, fill gaps)

On the lower lane:

| Do this | To |
|---|---|
| Drag across the waveform | select a range (snaps to the grid; hold **Alt** to drag freely) |
| Click | move the playhead there |
| **S** | split at the playhead (or at both edges of the selection) |
| **Delete** | silence the selection (hatched = silent on purpose) |
| Drag a clip's name strip | move the clip |
| Drag a clip's edge | trim it; **Alt**+drag the right edge to **stretch** it |
| Double-click a clip | select it |
| Ctrl+C / X / V, Ctrl+D | copy, cut, paste at the playhead, duplicate |
| **Snap** (above the lanes) or **G** | change or turn off the grid |

When a selection exists, a small bar appears above it:

* **New take** (or **F**): generates a new take in the same style (same prompt, type, tempo, key
  and track mode) and drops it into exactly that range when it arrives. Not right? Press it again,
  or use **From take** to put back the same bars from any earlier take.
* **From take**: the same bars from another take (all takes share the bar grid, so they line up).
* **Split**, **Delete**.

Right-click for everything else: clip level, *Use take N everywhere*, *Reset edits*. Every edit
is one undo step.

## T3. Listen

**Space** plays and stops; **Home** goes back to bar 1; **L** loops. **1 / 2 / 3** (or *Both /
Track / Sound*) choose what you hear; **M / S** and the small sliders are per-lane mute, solo and
level. The audition plays inside Context through the mixer insert it's on; it never ends up in an
FL Studio export.

*FL Studio note:* shortcuts reach Context when its window has focus (click an empty spot first).
Whether FL Studio passes every key (Space especially) to a plugin window hasn't been tested yet;
if Space starts FL Studio's song instead, use the play button and tell me what happened.

## T4. Process

**+ Add effect** → 20 effects. Click an effect to edit it below; drag to reorder (order = signal
flow); the power dot bypasses; right-click for Duplicate / Reset / Remove. Knobs: drag up/down,
double-click to reset, or double-click the value under it and type (e.g. `2.5k`, `-6`). A knob
drag is one undo step. **Processed | Original** (or **A**) compares with and without the rack.

## U. Drag the result into FL Studio

Drag **Drag to DAW** (bottom right), or the small *Drag to DAW* handle that appears at the top
right of the sound lane, onto the Playlist or a Channel. You get exactly what you hear (fit,
edits and effects) as a 24-bit WAV, exactly N bars long. Save / Show in folder / Copy path use
the same file. Processed files live in `%APPDATA%\Context\Generated\Processed` and are never
auto-deleted. History → **Load** brings any earlier generation back.

## U2. Manual QA checklist for FL Studio

- [ ] Plugin scan finds Context; it loads on the Master; window resizes smoothly
- [ ] Bridge connects (status dot top-right); licensing still activates
- [ ] Capture records 8 bars from FL Studio playback, starts on a bar line and stops by itself
- [ ] Generate at 2, 4, 6, 8 and 16 bars; each result starts on bar 1 with no dead space
- [ ] Send my track → Layer (Vocal): the vocal lines up with the captured bars
- [ ] Send my track → Restyle and Continue produce results; credits are used as expected
- [ ] Select a range → New take: the new take lands only in that range; From take swaps bars back
- [ ] Split, delete, move, trim and Alt-stretch clips; the sound plays as shown
- [ ] Ctrl+Z / Ctrl+Shift+Z walk through edits, fit changes, effects and knob drags
- [ ] Space / Home / L / 1 2 3 / ? work after clicking an empty spot in the plugin
- [ ] Drop a WAV and an MP3 onto the lower lane; both play and fit the selected bars
- [ ] Add, bypass, reorder, duplicate and remove effects; order changes the sound
- [ ] Drag to DAW: the clip in the Playlist sounds identical to the preview and is exactly N bars
- [ ] Save the project, close FL Studio, reopen: lanes, takes, edits, fit and effects come back
- [ ] Change the FL Studio sample rate with Context open: audio stays correct
- [ ] Settings → Reduce motion turns animations off
- [ ] Close the project with Context playing: no hang or crash

## V. Where logs are

`%APPDATA%\Context\Logs\context.log` — paste `%APPDATA%\Context` into the Explorer address bar.
Settings, history, generated sounds (`Generated`) and the license file live in that folder too.

## W. What to paste back into Claude if something fails

* **Build failed on GitHub:** the last ~50 lines of the red step in Actions.
* **Problem inside Context:** **Settings → Diagnostics → Copy Diagnostic Info**, then paste.
  (Secrets are removed automatically.) Add what you clicked and what you expected.
* **Licensing server problem:** the text in the `run-license-server` window.
* **Crash in FL Studio:** the last 50 lines of `context.log` plus FL Studio's crash message.
