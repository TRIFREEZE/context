#!/usr/bin/env python3
"""End-to-end test of tools/kie_bridge against a FAKE Kie.ai server (no account, no network).

1. Starts tools/kie_bridge/fake_kie_server.py and the bridge pointed at it.
2. Checks the bridge's Context Bridge Protocol v1 behaviour and the requests it sends to "Kie.ai".
3. If --tests-exe is given, runs `ContextTests --bridge-e2e=<bridge>` so Context's real
   LocalGenerationBridgeProvider + GenerationController generate through the bridge.

Used by CTest. Exit code 0 = pass.
"""
import argparse
import json
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
            rc = subprocess.call(cmd, timeout=300)
            check(rc == 0, "ContextTests --bridge-e2e passes")
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
