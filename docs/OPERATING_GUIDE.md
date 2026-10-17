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

## R. Generate a sound

Type what you want → pick the type (One Shot, Loop, Vocal, ...) → **GENERATE**.
The enhanced prompt under the type buttons shows exactly what will be sent.
Progress shows next to the button; **Cancel** stops it.

## S. Analyze a track

Play your song in FL Studio so audio flows through the mixer insert Context is on.
After a few seconds click **Analyze Track**. You'll see Tempo (from FL Studio), Key with
confidence, Low End / Mids / Highs, Stereo, Transients and Tone. With
**Use Track Analysis** ticked, the next generation is tailored to it. Nothing is uploaded.

## T. Preview a result

Click the waveform or **▶ Play** (plays at −6 dB through the same mixer insert);
**Stop** stops it. Previews never end up in an export/render.

## U. Drag the WAV into FL Studio

Press on the waveform and **drag it onto the Playlist** (or a Channel/Sampler). The file stays
in Context's folder and is protected from automatic cleanup once dragged.
If a drag doesn't work in a particular spot, use **Show in Folder** and drag from Explorer,
**Copy Path**, or **Save As...** to put a copy anywhere.

## V. Where logs are

`%APPDATA%\Context\Logs\context.log` — paste `%APPDATA%\Context` into the Explorer address bar.
Settings, history, generated sounds (`Generated`) and the license file live in that folder too.

## W. What to paste back into Claude if something fails

* **Build failed on GitHub:** the last ~50 lines of the red step in Actions.
* **Problem inside Context:** **Settings → Diagnostics → Copy Diagnostic Info**, then paste.
  (Secrets are removed automatically.) Add what you clicked and what you expected.
* **Licensing server problem:** the text in the `run-license-server` window.
* **Crash in FL Studio:** the last 50 lines of `context.log` plus FL Studio's crash message.
