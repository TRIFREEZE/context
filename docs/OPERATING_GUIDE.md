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

## R. The Create screen (Context V2)

```
┌ prompt ─────────────────────────────────────────────┐ [Generate]
  TYPE  One Shot · Loop · Vocal · ...   BARS 2 4 6 8 16   Analysis · Audio ctx · Enhance
├ TRACK CONTEXT ─┬ 1 ── 2 ── 3 ── 4 ── 5 ── 6 ── 7 ── 8 ─┤  ← your track (8 bars)
├ AI / SAMPLE ───┼───────────────────────────────────────┤  ← generated or dropped sound
  FIT  Auto · Fit · Trim · Loop · Repitch   [algorithm]   PITCH
  PROCESSING  [EQ] → [COMP] → [VERB]   + Add effect          Processed | Original
  ⏮ ▶ ⟲  Both · Context · Output   1.1 / 8 bars      ☆ Regenerate Variation  ⤓ 📁 ⧉  ⠿ Drag to DAW
```

Hover anything for a tooltip. Right-click a lane (or its ⋯ button) for more actions.

## S. Capture your track (TRACK CONTEXT)

1. Put Context on the FL Studio **Master** (or a bus everything goes through).
2. Click **Capture**. The lane says *Armed*.
3. Press play in FL Studio. Context starts on the next bar line and records **exactly 8 bars**,
   then stops by itself. The bar counter shows progress.
4. The waveform appears, with tempo and key in the lane header. The capture is analysed
   (key, low/mid/high balance) and — with **Analysis** on — shapes the next generation.

No DAW tempo (e.g. standalone)? Click the BPM in the lane header to type a tempo; capture then
starts when audio arrives. If you change the song tempo later, the header offers **Retarget**
(stretch the capture to the new tempo) or **Capture again**.
*Quick analyze* in the ⋯ menu analyses the last 8 seconds without a full capture.

## T. Generate, or drop your own sample (AI / SAMPLE)

* Type what you want, choose a **TYPE** and **BARS** (2/4/6/8/16), click **Generate**.
  The result lands in the lower lane, fitted to exactly that many bars at your tempo.
* Or drag a WAV / AIFF / FLAC / MP3 file from Explorer onto the lower lane (or ⋯ → *Load sample*).
  It goes through the same fitting and effects. *Restore generated version* brings back the AI result.
* **FIT**: *Auto* chooses for you; *Fit* time-stretches; *Trim* cuts; *Loop* repeats; *Repitch*
  speeds up/slows down like tape. The algorithm menu (Percussive, Monophonic, Polyphonic,
  Texture, Repitch) changes *how* it stretches. The text on the right says what was done.

## T2. Listen

**▶** plays both lanes in sync, looping. **Both / Context /
Output** choose what you hear; **M / S** and the small sliders are per-lane mute, solo and level.
This audition plays inside Context through the mixer insert it's on — it never ends up in an
FL Studio export.

## T3. Process (PROCESSING)

**+ Add effect** → pick from 20 effects (EQ, Filter, Compressor, Limiter, Gate, Transient,
Saturation, Distortion, Soft Clipper, Reverb, Delay, Chorus, Flanger, Phaser, Tremolo,
Auto-Pan, Bitcrusher, Downsampler, Utility, Stereo). Click an effect to edit it below;
drag chips to reorder (order = signal flow); the power dot bypasses; right-click for
Duplicate / Reset / Remove. Knobs: drag up/down, double-click the knob to reset, or double-click the value under it and type
(e.g. `2.5k`, `-6`).
**Processed | Original** compares with and without the rack. Nothing is ever written over
the original sound.

## U. Drag the result into FL Studio

Press **⠿ Drag to DAW** (or the lower waveform) and drag onto the Playlist or a Channel. You
get exactly what you hear: fitted to the bars, with the effects, as a 24-bit WAV that is
exactly N bars long. ⤓ *Save as...*, 📁 *Show in folder* and ⧉ *Copy path* use the same file.
Processed files live in `%APPDATA%\Context\Generated\Processed` and are never auto-deleted.
History → **Load** brings any earlier generation back into the workspace.

## U2. Manual QA checklist for FL Studio (V2)

Tick these in FL Studio after installing a new build:

- [ ] Plugin scan finds Context; it loads on the Master; window resizes smoothly
- [ ] Bridge connects (status dot top-right); licensing still activates
- [ ] Capture records 8 bars from FL Studio playback, starts on a bar line and stops by itself
- [ ] BPM and key appear in the TRACK CONTEXT header and match the project
- [ ] Generate at 2, 4, 6, 8 and 16 bars; each result is exactly that long (check the bar ruler)
- [ ] Drop a WAV and an MP3 onto the lower lane; both play and fit the selected bars
- [ ] Both / Context / Output, mute, solo and loop behave; lanes stay in sync over many loops
- [ ] Each FIT mode and stretch algorithm audibly changes the result
- [ ] Add, bypass, reorder, duplicate and remove effects; order changes the sound
- [ ] Processed | Original switches without a click
- [ ] Drag to DAW: the clip in the Playlist sounds identical to the preview and is exactly N bars
- [ ] Save the project, close FL Studio, reopen: lanes, fit settings and effects come back
- [ ] Change the FL Studio sample rate (Options → Audio) with Context open: audio stays correct
- [ ] Change the project tempo after capturing: *Retarget* / *Capture again* are offered
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
