# Kie.ai bridge — real AI audio in Context

`tools/kie_bridge/kie_bridge.py` is a bridge that connects Context to the **Kie.ai Suno API**
(https://docs.kie.ai). It runs on your computer at `http://127.0.0.1:3000` and speaks the
Context Bridge Protocol v1 (`BRIDGE_PROTOCOL.md`), so Context needs no profile and no changes.

```
Context (FL Studio) ──localhost──► Kie.ai bridge (this window) ──https + your API key──► api.kie.ai
        ◄──────────── WAV ─────────          (downloads + converts the result)
```

> Kie.ai is a **third-party** service. It is not Suno and is not affiliated with Suno.
> Read "Things to know before you rely on it" below.

## Setup (once)

1. Create a Kie.ai account, add credits, and copy your API key from the Kie.ai dashboard
   (**API Key** page).
2. Double-click **`scripts\run-kie-bridge.bat`**.
   * The first run sets up Python packages, which takes about a minute.
   * When it asks, paste the key and press Enter.
   * It checks the key and prints your remaining credits.
3. Leave that window open.
4. In Context, go to **Settings → Generation**:
   * Provider: **Local Generation Bridge**
   * Address: `http://127.0.0.1:3000`
   * Click **Test Connection**. It should show *✓ Local generation bridge detected (Kie.ai Suno Bridge)*.

The header dot turns green, not yellow. If it still says "Mock Provider", the Provider setting
wasn't changed.

Only one bridge can use port 3000. Close the reference (test-tone) bridge first.

**Change the key later:** run `scripts\change-kie-key.bat`.

## What gets generated

| You do in Context | Bridge asks Kie.ai for | Notes |
|---|---|---|
| Any type (One Shot, FX, Texture, Vocal, …) | **Suno Sounds** (`ai-music-api/sounds`) | Short sounds, samples and textures. |
| Type **Loop** | Sounds with `sound_loop` on | Seamless loop at your track's BPM. |
| Loop / Percussion / Melody with a known BPM | `sound_tempo` = your BPM, rounded | Comes from FL Studio's tempo. |
| A known key (after **Analyze Track**) | `sound_key`, e.g. `F#m` | Not sent for FX or Percussion. |
| Prompt contains **`#song`** | A full **Suno song** (`ai-music-api/generate`) | Instrumental unless the type is **Vocal**. The tag itself is removed from the prompt. |

* The prompt sent is Context's enhanced prompt, shortened to Kie.ai's limits: 500 characters
  for Sounds, 3000 for songs.
* Suno Sounds has **no length parameter** (the docs list none). Context asks for the bar length it
  needs, the bridge passes tempo and loop flags, and Context itself trims dead space and fits the
  result to exactly N bars.
* Model: V6. You can change it in `config.json`; see below.
* Kie.ai usually returns two takes. The bridge uses the first.
* Audio comes back to Context as **WAV**:
  * The bridge first asks Kie.ai for its WAV conversion. That may use a few extra credits.
  * If the conversion isn't available, the bridge decodes Kie.ai's MP3 to WAV itself, using the
    `miniaudio` package that the .bat installs.

### With your track ("Send my track" in Context)

Turn on **Send my track** in Context (it asks for permission once). Context then sends your
captured 8 bars to the bridge, and the bridge:

1. uploads them to Kie.ai's **File Upload API** (`kieai.redpandaai.co/api/file-stream-upload`;
   Kie.ai deletes uploads after 24 hours), then
2. runs one of these, depending on what you picked next to the toggle:

| Context setting | Type | Kie.ai steps | You get |
|---|---|---|---|
| **Layer** | Vocal, or a prompt that asks for vocals ("vocal chops" as a Loop) | `ai-music-api/add-vocals` on your bars, then `ai-music-api/separate-vocals` (2 stems) | only the new vocal, in time and in key with your track |
| **Layer** | anything else | `ai-music-api/upload-and-cover-audio` (instrumental, in the prompt's style), then `separate-vocals` with `split_stem` | the stem your prompt names: drums, bass, guitar, keys (piano, rhodes, bells), strings, brass, woodwinds, synth (pad, lead, pluck), percussion or FX. If the prompt names none, the whole result. |
| **Restyle** | any | `ai-music-api/upload-and-cover-audio` with `duration` = your bars (Kie.ai's minimum is 10 s) | your bars re-imagined in the prompt's style |
| **Continue** | any | `ai-music-api/upload-and-extend-audio` with `continue_at` = the end of your bars | what comes next. If Kie.ai returns your bars plus the continuation, the bridge detects that and cuts your part off. |

* **Credits:** every step is a paid Kie.ai task. A Layer request is two (generation + separation),
  plus the WAV conversion when it applies. Check your balance with `run-kie-bridge.bat --check`.
* **Timing:** Layer and Continue results start exactly on your bar 1 (or the bar after your
  capture), so Context keeps every sample where it is: no stretching, no silence trimming.
* **Settings** (in `config.json`): `context_audio_weight` (0–1, default `0.8`: how strongly Kie.ai
  follows your audio) and `negative_tags` (default `low quality, muffled, clipping`).
* **Long results:** Kie.ai can answer your 8 bars with a whole song (a user got 77.6 s) whose new
  part only starts after an intro. The bridge (1.1.1) keeps one context-length window on your bar
  grid: the first one with at least 90 % of the loudest window's sound. Its window says which bars
  it kept ("kept the vocal from bars 17-24").
* **Stem separation** is asked by task id + audio id first (the documented example). Kie.ai has
  answered "Record does not exist" for results made from uploaded audio, so the bridge then uses the
  documented `audio_url` input with the clip's link. If separation still fails, or the stem is
  silent, the bridge delivers the full result (the credits are already spent) and says so.
* **Errors name the step**, e.g. "Kie.ai add-vocals: ..." or "Kie.ai stem separation: ...".
* **What isn't known yet:** Kie.ai's docs don't say how long add-vocals results are or exactly how
  the stem files are named. The bridge accepts every documented result shape and falls back to the
  full result when the requested stem isn't there; its window says which stem it kept.

Generation takes roughly 20 s–3 min, depending on Kie.ai's queue. Context waits up to 10
minutes. **Cancel** stops the wait, but credits Kie.ai already used are not refunded.

## Settings file

`%APPDATA%\Context Kie Bridge\config.json`:

```json
{
  "api_key": "…",
  "sound_model": "V6",
  "music_model": "V6",
  "convert_to_wav": true
}
```

| Setting | Allowed values |
|---|---|
| `sound_model` | `V6`, `V6_MINI`, `V6_WILD`, `V5_5`, `V5` |
| `music_model` | `V6`, `V6_MINI`, `V6_WILD`, `V5_5`, `V5`, `V4_5ALL`, `V4_5PLUS`, `V4_5`, `V4` |
| `convert_to_wav` | `false` skips Kie.ai's WAV conversion and always decodes the MP3 locally. That saves credits but is lower quality. |
| `context_audio_weight` | `0`–`1` (default `0.8`). How closely track-context requests follow your audio. |
| `negative_tags` | Styles to avoid in track-context requests (Kie.ai requires the field). Default `low quality, muffled, clipping`. |

You can also use the `KIE_API_KEY` environment variable instead of the saved key.

## Security

* **Where the key lives.**
  * It is stored only in your Windows profile (above).
  * It is never stored in Context, the plugin, this project folder, GitHub, GitHub Actions or your FL Studio project.
  * It is sent only to `api.kie.ai` and is never printed. The bridge's own messages show at most the last 4 characters.
* **Who can use the bridge.**
  * It listens on `127.0.0.1` only.
  * It refuses requests that come from web pages, which carry an `Origin` or `Sec-Fetch-Site` header.
  * It refuses requests addressed to other host names, which blocks DNS rebinding. A website can't spend your credits.
* **What it accepts back.** It only accepts `https` download links from Kie.ai, caps downloads at
  100 MB, and checks every WAV before handing it to Context.
* **Cleanup.** Temporary audio is deleted after 24 hours.
* **Context's licensing server.** It never sees prompts, audio or the key (unchanged).

## Errors you may see in Context

| Context shows | Cause | Fix |
|---|---|---|
| "…couldn't generate this sound: Your Kie.ai account is out of credits." | No credits left | Top up on Kie.ai. |
| "…Kie.ai rejected the API key. Run the bridge with --set-key…" | Wrong or revoked key | Run `change-kie-key.bat`. |
| "The provider is busy or rate-limited." | Kie.ai rate limit | Wait a minute. |
| "…couldn't generate this sound: …sensitive…" | Kie.ai/Suno content filter | Reword the prompt. Artist names are often blocked. |
| "No bridge detected" | Bridge window closed, or a different port | Start `run-kie-bridge.bat`. |
| "The bridge lost track of this generation" | The bridge was restarted mid-generation | Generate again. |

The bridge window logs every step: request, Kie.ai state, WAV conversion, and ready or failed.
Check it first when something goes wrong.

## Things to know before you rely on it

* **Not official.**
  * Suno has no public API. In mid-2026 it announced it is *exploring* a curated API partner program.
  * Services like Kie.ai resell access to Suno models without Suno's endorsement.
  * Suno's Terms of Service (updated Aug 2026) prohibit reselling or granting access to the
    service, and bots and scraping. They also say that output obtained other than through an
    approved download channel may not be used commercially.
  * So whatever Kie.ai's own terms promise, the commercial rights to these sounds are
    **uncertain**.
* **Can break.** Kie.ai can change or lose access without notice. Generated files are kept by
  Kie.ai for 14 days. Context keeps its own copy, so that doesn't affect sounds you already have.
* **Selling Context with this.** Don't market Context as "powered by Suno" or bundle Kie.ai
  access. If Context is going to be a commercial product built on Suno, the safe route is
  Suno's API partner program.
* **Not legal advice.** If you plan to sell sounds made this way, check with a lawyer.

## How this was tested (and what wasn't)

Track context (V2.1): the fake Kie.ai server also implements the File Stream Upload API and the
add-vocals, upload-and-cover-audio, upload-and-extend-audio and separate-vocals models with the
field rules from docs.kie.ai, and the end-to-end test checks the upload, every mode's request
fields, stem selection, the continue-trim and refusal of a broken WAV. The real services were not
called.


* **Tested automatically** (`tests/integration/run_kie_bridge_e2e.py`, part of CTest and GitHub
  Actions). These tests run against a **fake Kie.ai server** that follows the request rules in
  Kie.ai's docs:
  * the protocol;
  * the security guards;
  * request mapping (sounds, loops, keys, BPM, `#song`, prompt limits);
  * WAV conversion, plus the MP3-decode fallback;
  * error mapping;
  * cancellation;
  * three documented result formats.

  The same test also drives Context's real bridge provider through the bridge (`ContextTests --bridge-e2e`).
* **Not tested:** the real `api.kie.ai`. The build environment has no Kie.ai account or internet
  access to it. The endpoints and fields come from https://docs.kie.ai as of Oct 2026, and the
  parsing accepts every result format those docs describe.

  If the first real generation fails, copy the bridge window's text. Then check
  `docs.kie.ai/market/common/get-task-detail` and `docs.kie.ai/suno-api/generate-sounds` for changes.
