#!/usr/bin/env python3
"""
TEST ONLY: a stand-in for api.kie.ai, used by tests/integration/run_kie_bridge_e2e.py so the
Kie.ai bridge can be tested without a real account, credits or network access.

It implements only the endpoints the bridge uses, with the request rules from
https://docs.kie.ai (createTask with model + input, jobs/recordInfo, chat/credit), and
validates the bodies it receives so a mapping mistake in the bridge shows up as a failure.

    python fake_kie_server.py --port 0 --key test-key --mp3 tests/fixtures/kie_test_tone.mp3
                              [--shape response|resultJson|sunoData]

Prompt triggers: #upfail #sensitive #credits #nowav #already #flakyup #badkey
"""
import argparse
import io
import json
import math
import struct
import threading
import uuid
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

SOUND_MODELS = {"V5", "V5_5", "V6", "V6_WILD", "V6_MINI"}
MUSIC_MODELS = {"V4", "V4_5", "V4_5PLUS", "V4_5ALL", "V5", "V5_5", "V6", "V6_MINI", "V6_WILD"}
KEYS = {"Any", "Cm", "C#m", "Dm", "D#m", "Em", "Fm", "F#m", "Gm", "G#m", "Am", "A#m", "Bm",
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}
CUSTOM_ONLY = {"title", "vocal_gender", "style_weight", "weirdness_constraint", "audio_weight", "variety", "duration"}

TASKS = {}
LOCK = threading.Lock()
LOG = []   # (model, input) of every accepted createTask, for the test to inspect


def wav_bytes(seconds=1.0, rate=44100):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(int(seconds * rate)):
            v = int(math.sin(2 * math.pi * 330 * i / rate) * 12000)
            frames += struct.pack("<hh", v, v)
        w.writeframes(bytes(frames))
    return buf.getvalue()


def validate(model, inp):
    if not isinstance(inp, dict):
        return "input must be an object"
    if model == "ai-music-api/sounds":
        if not isinstance(inp.get("prompt"), str) or not inp["prompt"].strip():
            return "prompt is required"
        if len(inp["prompt"]) > 500:
            return "prompt too long"
        if inp.get("model") not in SOUND_MODELS:
            return "model invalid"
        if "sound_loop" in inp and not isinstance(inp["sound_loop"], bool):
            return "sound_loop must be boolean"
        if "sound_tempo" in inp and (not isinstance(inp["sound_tempo"], int) or not 1 <= inp["sound_tempo"] <= 300):
            return "sound_tempo must be an integer 1-300"
        if "sound_key" in inp and inp["sound_key"] not in KEYS:
            return "sound_key invalid"
        unknown = set(inp) - {"prompt", "model", "sound_loop", "sound_tempo", "sound_key", "grab_lyrics"}
        return f"unknown fields {sorted(unknown)}" if unknown else None
    if model == "ai-music-api/generate":
        for f in ("custom_mode", "instrumental"):
            if not isinstance(inp.get(f), bool):
                return f"{f} must be boolean"
        if inp.get("model") not in MUSIC_MODELS:
            return "model invalid"
        if inp["custom_mode"] is False and CUSTOM_ONLY & set(inp):
            return "custom-only parameters sent in non-custom mode"
        if len(str(inp.get("prompt", ""))) > 3000:
            return "prompt too long"
        if not str(inp.get("prompt", "")).strip():
            return "prompt is required"
        return None
    if model == "ai-music-api/convert-to-wav-format":
        if not inp.get("task_id") or not inp.get("audio_id"):
            return "task_id and audio_id are required"
        return None
    return "unknown model"


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def js(self, obj, status=200):
        b = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def authed(self):
        if self.headers.get("Authorization") != "Bearer " + self.server.key:
            self.js({"code": 401, "msg": "You do not have access permissions"})
            return False
        return True

    def base(self):
        return f"http://127.0.0.1:{self.server.server_address[1]}"

    def do_GET(self):
        u = urlsplit(self.path)
        if u.path.startswith("/cdn/"):
            name = u.path[5:]
            if name.endswith(".mp3"):
                body, ctype = self.server.mp3, "audio/mpeg"
            elif name.endswith(".wav"):
                body, ctype = wav_bytes(), "audio/wav"
            else:
                return self.js({"error": "nf"}, 404)
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if u.path == "/_test/log":
            with LOCK:
                return self.js(list(LOG))
        if not self.authed():
            return
        if u.path == "/api/v1/chat/credit":
            return self.js({"code": 200, "msg": "success", "data": 1234.5})
        if u.path == "/api/v1/jobs/recordInfo":
            tid = (parse_qs(u.query).get("taskId") or [""])[0]
            with LOCK:
                t = TASKS.get(tid)
                if t:
                    t["polls"] += 1
            if not t:
                return self.js({"code": 422, "msg": "recordInfo is null"})
            return self.record(tid, t)
        return self.js({"code": 404, "msg": "not found"})

    def record(self, tid, t):
        p = t["prompt"]
        if "#flakyup" in p and t["polls"] <= 2:
            return self.js({"code": 500, "msg": "server busy"})
        states = ["waiting", "queuing", "generating", "generating"]
        if t["polls"] <= len(states):
            return self.js({"code": 200, "msg": "success",
                            "data": {"taskId": tid, "model": t["model"], "state": states[t["polls"] - 1],
                                     "resultJson": "", "failCode": "", "failMsg": "", "progress": t["polls"] * 20}})
        data = {"taskId": tid, "model": t["model"], "state": "success", "failCode": "", "failMsg": ""}
        if "#upfail" in p:
            data.update(state="fail", failCode="500", failMsg="internal generation error")
        elif "#sensitive" in p:
            data.update(state="fail", failCode="400", failMsg="SENSITIVE_WORD_ERROR: prompt contains sensitive words")
        elif t["model"] == "ai-music-api/convert-to-wav-format":
            data["resultJson"] = json.dumps({"resultUrls": [f"{self.base()}/cdn/{t['audio_id']}.wav"]})
        else:
            clips = [{"id": t["clip_ids"][i], "audio_url": f"{self.base()}/cdn/{t['clip_ids'][i]}.mp3",
                      "stream_audio_url": f"{self.base()}/stream/{t['clip_ids'][i]}", "image_url": "",
                      "duration": 1.5, "title": "x", "tags": "x"} for i in range(2)]
            shape = self.server.shape
            if shape == "response":
                data["response"] = {"taskId": tid, "data": clips}
            elif shape == "resultJson":
                data["resultJson"] = json.dumps({"resultUrls": [c["audio_url"] for c in clips], "data": clips})
            else:  # legacy camelCase
                data["response"] = {"taskId": tid, "sunoData": [{"id": c["id"], "audioUrl": c["audio_url"],
                                                                  "duration": 1.5} for c in clips]}
        return self.js({"code": 200, "msg": "success", "data": data})

    def do_POST(self):
        if not self.authed():
            return
        if urlsplit(self.path).path != "/api/v1/jobs/createTask":
            return self.js({"code": 404, "msg": "not found"})
        try:
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)))
        except ValueError:
            return self.js({"code": 422, "msg": "invalid json"})
        if set(body) - {"model", "input", "callBackUrl"}:
            return self.js({"code": 422, "msg": "unexpected top-level fields"})
        model, inp = body.get("model"), body.get("input")
        err = validate(model, inp)
        if err:
            return self.js({"code": 422, "msg": err})
        prompt = str(inp.get("prompt", ""))
        if model == "ai-music-api/convert-to-wav-format":
            with LOCK:
                parent = TASKS.get(inp["task_id"])
            if not parent or inp["audio_id"] not in parent["clip_ids"]:
                return self.js({"code": 422, "msg": "task_id/audio_id not found"})
            prompt = parent["prompt"]
            if "#nowav" in prompt:
                return self.js({"code": 500, "msg": "conversion service unavailable"})
            if "#already" in prompt:
                return self.js({"code": 422, "msg": "already converted",
                                "data": {"taskId": "old", "audioWavUrl": f"{self.base()}/cdn/{inp['audio_id']}.wav"}})
        if "#credits" in prompt:
            return self.js({"code": 402, "msg": "Insufficient credits"})
        tid = uuid.uuid4().hex
        with LOCK:
            TASKS[tid] = {"model": model, "prompt": prompt, "polls": 0,
                          "clip_ids": [str(uuid.uuid4()), str(uuid.uuid4())], "audio_id": inp.get("audio_id")}
            LOG.append({"model": model, "input": inp})
        return self.js({"code": 200, "msg": "success", "data": {"taskId": tid}})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--key", default="test-key")
    ap.add_argument("--mp3", required=True)
    ap.add_argument("--shape", default="response", choices=["response", "resultJson", "sunoData"])
    a = ap.parse_args()
    s = ThreadingHTTPServer(("127.0.0.1", a.port), H)
    s.daemon_threads = True
    s.key, s.shape = a.key, a.shape
    with open(a.mp3, "rb") as f:
        s.mp3 = f.read()
    print(f"PORT {s.server_address[1]}", flush=True)
    s.serve_forever()


if __name__ == "__main__":
    main()
