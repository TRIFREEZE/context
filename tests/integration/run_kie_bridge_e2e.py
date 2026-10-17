#!/usr/bin/env python3
"""End-to-end test of tools/kie_bridge against a FAKE Kie.ai server (no account, no network).

1. Starts tools/kie_bridge/fake_kie_server.py and the bridge pointed at it.
2. Checks the bridge's Context Bridge Protocol v1 behaviour and the requests it sends to "Kie.ai".
3. If --tests-exe is given, runs `ContextTests --bridge-e2e=<bridge>` so Context's real
   LocalGenerationBridgeProvider + GenerationController generate through the bridge.

Used by CTest. Exit code 0 = pass.
"""
import argparse
import base64
import io
import json
import math
import struct
import wave
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BRIDGE = os.path.join(ROOT, "tools", "kie_bridge", "kie_bridge.py")
FAKE = os.path.join(ROOT, "tools", "kie_bridge", "fake_kie_server.py")
MP3 = os.path.join(ROOT, "tests", "fixtures", "kie_test_tone.mp3")

FAILURES = []
PROCS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILURES.append(what)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start_fake(shape):
    p = subprocess.Popen([sys.executable, FAKE, "--port", "0", "--key", "test-key", "--mp3", MP3, "--shape", shape],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    PROCS.append(p)
    line = p.stdout.readline()
    if not line.startswith("PORT "):
        raise RuntimeError("fake Kie server didn't start: " + line + p.stdout.read())
    return f"http://127.0.0.1:{int(line.split()[1])}"


def start_bridge(api_base, key="test-key"):
    port = free_port()
    cfg = tempfile.mkdtemp(prefix="kie-bridge-cfg-")
    env = dict(os.environ, KIE_API_KEY=key, PYTHONUNBUFFERED="1")
    log_path = os.path.join(cfg, "bridge.log")
    out = open(log_path, "w")
    p = subprocess.Popen([sys.executable, BRIDGE, "--port", str(port), "--config-dir", cfg,
                          "--api-base", api_base, "--fast-poll"],
                         stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT, env=env)
    p.log_path = log_path
    PROCS.append(p)
    url = f"http://127.0.0.1:{port}"
    for _ in range(100):
        try:
            urllib.request.urlopen(url + "/context/v1/health", timeout=1).close()
            return url, p
        except Exception:
            if p.poll() is not None:
                raise RuntimeError("bridge exited early:\n" + open(log_path).read())
            time.sleep(0.1)
    raise RuntimeError("bridge did not start")


def http(method, url, body=None, headers=None, raw=False):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            payload = r.read()
            return r.status, (payload if raw else json.loads(payload or b"null"))
    except urllib.error.HTTPError as e:
        payload = e.read()
        try:
            return e.code, json.loads(payload)
        except ValueError:
            return e.code, payload


def generate(bridge, prompt, ctype="one_shot", wait=True, **extra):
    body = {"protocol": "context-bridge-v1", "requestId": "t", "prompt": prompt, "userPrompt": prompt,
            "type": ctype, **extra}
    st, r = http("POST", bridge + "/context/v1/generate", body)
    if st not in (200, 202) or not wait:
        return st, r
    job = r["jobId"]
    for _ in range(300):
        st, s = http("GET", f"{bridge}/context/v1/jobs/{job}")
        if s.get("status") in ("complete", "failed", "cancelled"):
            return st, s
        time.sleep(0.1)
    return st, {"status": "timeout"}


def fetch_wav(bridge, status):
    st, data = http("GET", bridge + status["audioUrl"], raw=True)
    return st == 200 and data[:4] == b"RIFF" and data[8:12] == b"WAVE", data


def upstream_log(api):
    return http("GET", api + "/_test/log")[1]


def pulse_wav(seconds=4.0, rate=44100, bpm=120):
    """A stereo 16-bit 'beat': decaying 120 Hz hits on every quarter note (varied envelope)."""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        beat = 60.0 / bpm
        frames = bytearray()
        for i in range(int(seconds * rate)):
            t = i / rate
            ph = t % beat
            amp = math.exp(-ph * 18.0) * (1.0 if int(t / beat) % 2 == 0 else 0.55)
            v = int(math.sin(2 * math.pi * 120 * t) * 20000 * amp)
            frames += struct.pack("<hh", v, v)
        w.writeframes(bytes(frames))
    return buf.getvalue()


def wav_seconds(data):
    with wave.open(io.BytesIO(data)) as w:
        return w.getnframes() / float(w.getframerate())


def have_miniaudio():
    return subprocess.call([sys.executable, "-c", "import miniaudio"], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL) == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tests-exe")
    args = ap.parse_args()
    mp3_ok = have_miniaudio()
    print(f"miniaudio available: {mp3_ok}")

    try:
        api = start_fake("response")
        bridge, bproc = start_bridge(api)

        print("Protocol")
        st, h = http("GET", bridge + "/context/v1/health")
        check(st == 200 and h.get("protocol") == "context-bridge-v1", "health speaks context-bridge-v1")
        st, c = http("GET", bridge + "/context/v1/capabilities")
        check(st == 200 and "one_shot" in c.get("generationTypes", []) and "cancellation" in c.get("features", []),
              "capabilities list types + cancellation")
        st, _ = http("GET", bridge + "/context/v1/generate")
        check(st == 405, "GET on generate is 405 (Context's 'endpoint exists' probe)")

        print("Security guards")
        st, _ = http("GET", bridge + "/context/v1/health", headers={"Origin": "https://evil.example"})
        check(st == 403, "requests with an Origin header (web pages) are refused")
        st, _ = http("GET", bridge + "/context/v1/health", headers={"Host": "evil.example:3000"})
        check(st == 403, "foreign Host header (DNS rebinding) is refused")
        st, _ = http("OPTIONS", bridge + "/context/v1/generate")
        check(st == 403, "CORS preflight is refused")
        req = urllib.request.Request(bridge + "/context/v1/generate", data=b'{"protocol":"context-bridge-v1"}',
                                     method="POST", headers={"Content-Type": "text/plain"})
        try:
            urllib.request.urlopen(req, timeout=5)
            st = 200
        except urllib.error.HTTPError as e:
            st = e.code
        check(st == 415, "non-JSON content type is refused")
        st, _ = http("GET", bridge + "/files/../../etc/passwd.wav")
        check(st == 404, "path traversal on /files is refused")
        log_text = open(bproc.log_path).read()
        check("test-key" not in log_text, "API key never appears in the bridge's output")

        print("Sounds (default)")
        before = len(upstream_log(api))
        st, s = generate(bridge, "short punchy trap kick", "one_shot", bpm=140, key="F#", mode="minor")
        check(s.get("status") == "complete", f"one-shot completes ({s})")
        if s.get("status") == "complete":
            ok, data = fetch_wav(bridge, s)
            check(ok and len(data) > 1000, "audio is served as WAV")
        sent = upstream_log(api)[before:]
        check(len(sent) >= 1 and sent[0]["model"] == "ai-music-api/sounds", "one-shot uses ai-music-api/sounds")
        if sent:
            inp = sent[0]["input"]
            check(inp.get("model") == "V6" and inp.get("sound_key") == "F#m", f"model V6 + key F#m ({inp})")
            check("sound_tempo" not in inp and "sound_loop" not in inp, "no tempo/loop for a one-shot")
            check(any(x["model"] == "ai-music-api/convert-to-wav-format" for x in sent), "asks Kie.ai for a WAV")

        before = len(upstream_log(api))
        st, s = generate(bridge, "dusty drum loop", "loop", bpm=92.6, key="Bb", mode="major")
        check(s.get("status") == "complete", "loop completes")
        sent = upstream_log(api)[before:]
        if sent:
            inp = sent[0]["input"]
            check(inp.get("sound_loop") is True and inp.get("sound_tempo") == 93 and inp.get("sound_key") == "A#",
                  f"loop sends sound_loop, rounded tempo, flat key mapped to sharp ({inp})")

        before = len(upstream_log(api))
        st, s = generate(bridge, "riser " + "very long words " * 80, "fx", key="C", mode="minor")
        sent = upstream_log(api)[before:]
        check(s.get("status") == "complete" and sent and len(sent[0]["input"]["prompt"]) <= 500
              and "sound_key" not in sent[0]["input"], "long FX prompt is shortened to 500 chars, no key for FX")

        print("Songs (#song)")
        before = len(upstream_log(api))
        st, s = generate(bridge, "dark trap beat #song", "loop")
        sent = upstream_log(api)[before:]
        check(s.get("status") == "complete", "#song completes")
        check(sent and sent[0]["model"] == "ai-music-api/generate" and sent[0]["input"]["instrumental"] is True
              and sent[0]["input"]["custom_mode"] is False and "#song" not in sent[0]["input"]["prompt"],
              f"#song uses ai-music-api/generate, instrumental, tag removed ({sent[:1]})")
        before = len(upstream_log(api))
        st, s = generate(bridge, "#song soulful hook", "vocal")
        sent = upstream_log(api)[before:]
        check(sent and sent[0]["input"]["instrumental"] is False, "vocal type song has vocals")

        print("WAV conversion fallbacks")
        st, s = generate(bridge, "kick #already")
        check(s.get("status") == "complete", "already-converted (422 + existing link) still works")
        st, s = generate(bridge, "kick #nowav")
        if mp3_ok:
            check(s.get("status") == "complete", "conversion failure falls back to decoding the MP3")
            if s.get("status") == "complete":
                ok, data = fetch_wav(bridge, s)
                check(ok and len(data) > 100000, "decoded MP3 is a full-length WAV")
        else:
            check(s.get("status") == "failed" and "miniaudio" in s.get("error", ""),
                  "without the MP3 decoder the error explains how to install it")

        print("Errors")
        st, s = generate(bridge, "kick #upfail")
        check(s.get("status") == "failed" and "internal generation error" in s.get("error", ""), f"upstream failure ({s})")
        st, s = generate(bridge, "kick #sensitive")
        check(s.get("status") == "failed" and s.get("errorCode") == "content_filtered", f"content filter -> content_filtered ({s})")
        st, s = generate(bridge, "kick #credits")
        check(s.get("status") == "failed" and s.get("errorCode") == "kie_out_of_credits" and "credits" in s.get("error", ""),
              f"no credits -> kie_out_of_credits ({s})")
        st, s = generate(bridge, "kick #flakyup")
        check(s.get("status") == "complete", "transient Kie.ai 500s while polling are retried")
        st, s = generate(bridge, "kick", wait=False)
        job = s["jobId"]
        http("POST", f"{bridge}/context/v1/jobs/{job}/cancel")
        time.sleep(1.0)
        st, s = http("GET", f"{bridge}/context/v1/jobs/{job}")
        check(s.get("status") == "cancelled", "cancel works")
        st, s = http("GET", f"{bridge}/context/v1/jobs/doesnotexist")
        check(st == 404, "unknown job is 404")
        st, s = http("POST", bridge + "/context/v1/generate", {"protocol": "context-bridge-v1", "prompt": "  "})
        check(st == 400, "empty prompt is rejected")

        print("Track context (audio conditioning)")
        st, c = http("GET", bridge + "/context/v1/capabilities")
        check("audio_conditioning" in c.get("features", []) and c.get("contextModes") == ["layer", "cover", "extend"],
              f"capabilities announce audio_conditioning + context modes ({c})")
        ctx_wav = pulse_wav(4.0)
        actx = {"format": "wav", "encoding": "base64", "data": base64.b64encode(ctx_wav).decode(), "bars": 2}

        before = len(upstream_log(api))
        st, s = generate(bridge, "dark distorted female vocal chop", "vocal", audioContext=actx, contextMode="layer")
        sent = upstream_log(api)[before:]
        models = [x["model"] for x in sent]
        check(models[:3] == ["upload", "ai-music-api/add-vocals", "ai-music-api/separate-vocals"],
              f"vocal layer: upload -> add-vocals -> separate-vocals ({models})")
        if len(sent) >= 3:
            av = sent[1]["input"]
            check(av.get("vocal_gender") == "f" and av.get("style") and av.get("title") and av.get("negative_tags")
                  and "/files/context-bridge/" in av.get("upload_url", ""),
                  f"add-vocals gets the uploaded context, style, title, gender ({av})")
            sep = sent[2]["input"]
            check(sep.get("type") == "separate_vocal" and "/cdn/" in str(sep.get("audio_url", "")) and "audio_id" not in sep,
                  f"Kie refuses to separate an add-vocals result by task id ('Record does not exist'), so the bridge "
                  f"separates the result's audio link, 2-stem mode ({sep})")
            blog = open(bproc.log_path).read()
            check("stem separation by task id refused (Record does not exist); trying the audio link" in blog,
                  "the bridge window says why it switched to the audio link")
            check(sent[0]["input"]["bytes"] == len(ctx_wav), "the exact context WAV is uploaded")
        if mp3_ok:
            check(s.get("status") == "complete", f"vocal layer completes ({s})")
            if s.get("status") == "complete":
                ok, data = fetch_wav(bridge, s)
                check(ok, "vocal stem is served as WAV")
        else:
            check(s.get("status") == "failed" and "miniaudio" in s.get("error", ""), "stems need the MP3 decoder")

        before = len(upstream_log(api))
        st, s = generate(bridge, "dark piano melody", "instrument", audioContext=actx, contextMode="layer")
        sent = upstream_log(api)[before:]
        models = [x["model"] for x in sent]
        check(models[:3] == ["upload", "ai-music-api/upload-and-cover-audio", "ai-music-api/separate-vocals"],
              f"instrument layer: upload -> cover -> split stems ({models})")
        if len(sent) >= 3:
            cv = sent[1]["input"]
            check(cv.get("instrumental") is True and cv.get("duration") == 10 and "prompt" not in cv,
                  f"cover is instrumental, duration clamped to Kie's 10 s minimum ({cv})")
            check(sent[2]["input"].get("type") == "split_stem" and sent[2]["input"].get("audio_url"),
                  "instrument layer separates the cover's audio link into stems")
        if mp3_ok:
            check(s.get("status") == "complete", f"instrument layer completes ({s})")
            check("keeping the keyboard stem" in open(bproc.log_path).read(), "'piano' picks the keyboard stem")

        # "Vocal chops" asked for as a Loop is still a vocal: add-vocals, never an instrumental cover
        # (whose vocal stem is silence - the blank result a user got).
        before = len(upstream_log(api))
        st, s = generate(bridge, "dark vocal chops", "loop", audioContext=actx, contextMode="layer")
        models = [x["model"] for x in upstream_log(api)[before:]]
        check(models[:3] == ["upload", "ai-music-api/add-vocals", "ai-music-api/separate-vocals"],
              f"a Loop of vocal chops is layered with add-vocals ({models})")

        # Kie.ai returns a long song whose vocal starts late: keep the bars with the vocal, on the grid.
        st, s = generate(bridge, "female vocal hook #late", "vocal", audioContext=actx, contextMode="layer")
        check(s.get("status") == "complete", f"late-vocal layer completes ({s})")
        if s.get("status") == "complete":
            ok, data = fetch_wav(bridge, s)
            secs = wav_seconds(data) if ok else 0
            check(ok and abs(secs - 4.0) < 0.02, f"one context length is kept ({secs:.3f} s)")
            if ok:
                with wave.open(io.BytesIO(data)) as w:
                    frames = w.readframes(w.getnframes())
                peak = max(abs(int.from_bytes(frames[i:i + 2], "little", signed=True)) for i in range(0, len(frames), 2 * 97))
                check(peak > 3000, f"the kept part has the vocal in it, not the silent intro (peak {peak})")
            check("lined it up from its bar 11" in open(bproc.log_path).read(),
                  "the bridge window says where the part was lined up from")

        # The vocal only in the last 0.3 s of the answer: moved to the bar line before it.
        st, s = generate(bridge, "breathy vocal #tail", "vocal", audioContext=actx, contextMode="layer")
        if s.get("status") == "complete":
            ok, data = fetch_wav(bridge, s)
            secs = wav_seconds(data) if ok else 0
            with wave.open(io.BytesIO(data)) as w:
                frames = w.readframes(w.getnframes())
            n = len(frames) // 4
            head = max(abs(int.from_bytes(frames[i * 4:i * 4 + 2], "little", signed=True)) for i in range(0, n // 2, 13))
            check(ok and abs(secs - 2.1) < 0.05, f"kept from the bar line before the vocal ({secs:.2f} s)")
            check(head == 0 and "lined it up from its bar 2" in open(bproc.log_path).read(),
                  "starts on bar 2's line, the vocal 1.8 s in (it lines up with the beat, not shifted off-grid)")
        else:
            check(False, f"tail vocal layer completes ({s})")

        # Separation refused by both routes: the paid generation is still delivered (whole mix).
        if mp3_ok:
            st, s = generate(bridge, "soft male vocal #nosep", "vocal", audioContext=actx, contextMode="layer")
            check(s.get("status") == "complete", f"if Kie.ai can't separate at all, the full result is still delivered ({s})")
            check("couldn't separate the vocal" in open(bproc.log_path).read(), "the bridge window says the result includes the track")

        # What the producer passed on reaches Kie.ai as negative tags (merged with the config's).
        before = len(upstream_log(api))
        generate(bridge, "airy female vocal", "vocal", audioContext=actx, contextMode="layer", negativeTags="busy, cluttered")
        sent = [x for x in upstream_log(api)[before:] if x["model"] == "ai-music-api/add-vocals"]
        neg = sent[0]["input"].get("negative_tags", "") if sent else ""
        check(neg.startswith("busy, cluttered") and "low quality" in neg and len(neg) <= 200,
              f"request negativeTags are merged into negative_tags ({neg})")

        # Errors name the step that failed.
        st, s = generate(bridge, "vocal #upfail", "vocal", audioContext=actx, contextMode="layer")
        check(s.get("status") == "failed" and s.get("error", "").startswith("Kie.ai add-vocals:"),
              f"a failed step is named in the error ({s.get('error')})")

        before = len(upstream_log(api))
        st, s = generate(bridge, "lofi tape version", "loop", audioContext=actx, contextMode="cover")
        sent = upstream_log(api)[before:]
        models = [x["model"] for x in sent]
        check(s.get("status") == "complete" and "ai-music-api/separate-vocals" not in models
              and "ai-music-api/upload-and-cover-audio" in models, f"restyle = one cover, no separation ({models})")

        before = len(upstream_log(api))
        st, s = generate(bridge, "keep the groove going", "loop", audioContext=actx, contextMode="extend")
        sent = upstream_log(api)[before:]
        ext = [x for x in sent if x["model"] == "ai-music-api/upload-and-extend-audio"]
        check(bool(ext) and 0 < ext[0]["input"]["continue_at"] < 4.0 and ext[0]["input"]["instrumental"] is True,
              f"continue uses upload-and-extend with continue_at inside the context ({ext[:1]})")
        check(s.get("status") == "complete", f"continue completes ({s})")
        if s.get("status") == "complete":
            ok, data = fetch_wav(bridge, s)
            secs = wav_seconds(data) if ok else 0
            check(ok and 2.5 < secs < 3.6, f"result that repeats the context first is cut to the new part only ({secs:.2f} s)")

        bad = dict(actx, data=base64.b64encode(b"not a wav at all").decode())
        st, r = http("POST", bridge + "/context/v1/generate",
                     {"protocol": "context-bridge-v1", "prompt": "x", "type": "vocal", "audioContext": bad})
        check(st == 400 and r.get("errorCode") == "audio_context_rejected", f"broken context WAV is refused ({st} {r})")
        log_text = open(bproc.log_path).read()
        check("test-key" not in log_text, "API key still never appears in the bridge's output")

        print("Wrong API key")
        bad_bridge, _ = start_bridge(api, key="wrong-key-123")
        st, s = generate(bad_bridge, "kick")
        check(s.get("status") == "failed" and s.get("errorCode") == "kie_api_key_rejected" and "--set-key" in s.get("error", ""),
              f"invalid key -> kie_api_key_rejected ({s})")

        print("Other documented result shapes")
        for shape in ("resultJson", "sunoData"):
            api2 = start_fake(shape)
            b2, _ = start_bridge(api2)
            st, s = generate(b2, "snare")
            check(s.get("status") == "complete", f"result shape '{shape}' is understood ({s})")

        if args.tests_exe:
            print("Context plugin code (LocalGenerationBridgeProvider) through the bridge")
            cmd = [args.tests_exe, f"--bridge-e2e={bridge}"]
            print("Running:", " ".join(cmd), flush=True)
            before = len(open(bproc.log_path).read())
            env = dict(os.environ, CONTEXT_E2E_MP3="1" if have_miniaudio() else "0")
            rc = subprocess.call(cmd, timeout=300, env=env)
            check(rc == 0, "ContextTests --bridge-e2e passes")
            plugin_log = open(bproc.log_path).read()[before:]
            check("s of track context" in plugin_log and "uploading" in plugin_log,
                  "the plugin's Send my track requests reached the bridge with the track and were uploaded to (fake) Kie.ai")
            check("test-key" not in plugin_log, "API key never appears in the bridge's output")
    finally:
        for p in PROCS:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()

    print()
    print(f"==== {len(FAILURES)} failure(s) ====")
    for f in FAILURES:
        print("  - " + f)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
