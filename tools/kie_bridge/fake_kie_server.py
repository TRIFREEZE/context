#!/usr/bin/env python3
"""
TEST ONLY: a stand-in for api.kie.ai, used by tests/integration/run_kie_bridge_e2e.py so the
Kie.ai bridge can be tested without a real account, credits or network access.

It implements only the endpoints the bridge uses, with the request rules from
https://docs.kie.ai (createTask with model + input, jobs/recordInfo, chat/credit, the File Stream
Upload API, and the add-vocals / upload-and-cover-audio / upload-and-extend-audio /
separate-vocals models), and validates the bodies it receives so a mapping mistake in the
bridge shows up as a failure.

    python fake_kie_server.py --port 0 --key test-key --mp3 tests/fixtures/kie_test_tone.mp3
                              [--shape response|resultJson|sunoData]

Prompt triggers: #upfail #sensitive #credits #nowav #already #flakyup #badkey #nosep #late
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

V6_FAMILY = {"V6", "V6_MINI", "V6_WILD"}
UPLOAD_MODELS = {"V4_5PLUS", "V5", "V5_5", "V6", "V6_MINI", "V6_WILD"}
STEMS_2 = ["Vocals", "Instrumental"]
UPLOAD_TASK_MODELS = {"ai-music-api/add-vocals", "ai-music-api/upload-and-cover-audio", "ai-music-api/upload-and-extend-audio"}
STEMS_12 = ["Vocals", "Backing Vocals", "Drums", "Bass", "Guitar", "Keyboard", "Strings", "Brass",
            "Woodwinds", "Percussion", "Synth", "FX"]

TASKS = {}
UPLOADS = {}   # file name -> bytes
LOCK = threading.Lock()
LOG = []   # (model, input) of every accepted createTask, for the test to inspect
UPLOADS_SECONDS = {}


def wav_bytes(seconds=1.0, rate=44100, freq=330):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(int(seconds * rate)):
            v = int(math.sin(2 * math.pi * freq * i / rate) * 12000)
            frames += struct.pack("<hh", v, v)
        w.writeframes(bytes(frames))
    return buf.getvalue()


def concat_wav(a: bytes, b: bytes) -> bytes:
    """a followed by b (both 16-bit; b is resampled by frame-dropping if the rates differ: test only)."""
    with wave.open(io.BytesIO(a)) as wa, wave.open(io.BytesIO(b)) as wb:
        ch, rate = wa.getnchannels(), wa.getframerate()
        fa = wa.readframes(wa.getnframes())
        fb = wb.readframes(wb.getnframes())
        if wb.getnchannels() != ch:                      # stereo tone -> mono
            fb = b"".join(fb[i:i + 2] for i in range(0, len(fb), 4)) if ch == 1 else fb
    out = io.BytesIO()
    with wave.open(out, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(fa + fb)
    return out.getvalue()


def _weights(inp):
    for f in ("style_weight", "weirdness_constraint", "audio_weight"):
        if f in inp and (not isinstance(inp[f], (int, float)) or not 0 <= inp[f] <= 1 or round(inp[f], 2) != inp[f]):
            return f"{f} must be 0-1 with at most 2 decimals"
    if "variety" in inp and (not isinstance(inp["variety"], int) or not 0 <= inp["variety"] <= 4):
        return "variety must be an integer 0-4"
    if "vocal_gender" in inp and inp["vocal_gender"] not in ("m", "f"):
        return "vocal_gender must be m or f"
    return None


def _uploaded(url, base):
    return isinstance(url, str) and url.startswith(base + "/files/") and url[len(base) + 7:] in UPLOADS


def validate(model, inp, base=""):
    if not isinstance(inp, dict):
        return "input must be an object"
    if model == "ai-music-api/add-vocals":
        allowed = {"upload_url", "title", "negative_tags", "style", "prompt", "lyrics", "model", "vocal_gender",
                   "style_weight", "weirdness_constraint", "audio_weight", "variety"}
        if set(inp) - allowed:
            return f"unknown fields {sorted(set(inp) - allowed)}"
        if not _uploaded(inp.get("upload_url"), base):
            return "upload_url is not a downloadable file"
        for f, lim in (("title", 10 ** 6), ("negative_tags", 200), ("style", 1000)):
            if not isinstance(inp.get(f), str) or not inp[f] or len(inp[f]) > lim:
                return f"{f} is required (max {lim})"
        if "model" in inp and inp["model"] not in UPLOAD_MODELS:
            return "model invalid"
        return _weights(inp)
    if model == "ai-music-api/upload-and-cover-audio":
        allowed = {"upload_url", "instrumental", "model", "lyrics", "prompt", "style", "title", "negative_tags",
                   "vocal_gender", "style_weight", "weirdness_constraint", "audio_weight", "variety", "persona_id",
                   "persona_model", "duration"}
        if set(inp) - allowed:
            return f"unknown fields {sorted(set(inp) - allowed)}"
        if not _uploaded(inp.get("upload_url"), base):
            return "upload_url is not a downloadable file"
        if not isinstance(inp.get("instrumental"), bool):
            return "instrumental is required"
        if inp["instrumental"] and {"lyrics", "prompt", "vocal_gender"} & set(inp):
            return "lyrics/prompt/vocal_gender not allowed when instrumental"
        if inp.get("model") not in V6_FAMILY:
            return "model invalid"
        if len(str(inp.get("style", ""))) > 1000 or len(str(inp.get("title", ""))) > 80:
            return "style/title too long"
        if "duration" in inp and (not isinstance(inp["duration"], int) or not 10 <= inp["duration"] <= 360):
            return "duration must be an integer 10-360"
        return _weights(inp)
    if model == "ai-music-api/upload-and-extend-audio":
        allowed = {"upload_url", "model", "continue_at", "instrumental", "lyrics", "prompt", "style", "title",
                   "negative_tags", "vocal_gender", "style_weight", "weirdness_constraint", "audio_weight",
                   "variety", "persona_id", "persona_model"}
        if set(inp) - allowed:
            return f"unknown fields {sorted(set(inp) - allowed)}"
        if not _uploaded(inp.get("upload_url"), base):
            return "upload_url is not a downloadable file"
        if inp.get("model") not in V6_FAMILY:
            return "model invalid"
        ca = inp.get("continue_at")
        seconds = UPLOADS_SECONDS.get(inp["upload_url"][len(base) + 7:], 0)
        if ca is not None and (not isinstance(ca, (int, float)) or not 0 < ca < seconds):
            return f"continue_at must be > 0 and < the audio length ({seconds:.2f})"
        if inp.get("instrumental") and {"lyrics", "prompt", "vocal_gender"} & set(inp):
            return "lyrics/prompt/vocal_gender not allowed when instrumental"
        if len(str(inp.get("title", ""))) > 100:
            return "title too long"
        return _weights(inp)
    if model == "ai-music-api/separate-vocals":
        if set(inp) - {"task_id", "audio_id", "audio_url", "type", "stem_name"}:
            return "unknown fields"
        # docs.kie.ai/suno-api/separate-vocals: task_id + audio_id, or audio_url (never both ids and a url).
        if "audio_url" in inp:
            if "audio_id" in inp:
                return "audio_url cannot be used together with audio_id"
            if not str(inp["audio_url"]).startswith(("https://", "http://")):
                return "audio_url must be a URL"
        elif not inp.get("task_id") or not inp.get("audio_id"):
            return "task_id + audio_id (or audio_url) required"
        if inp.get("type", "separate_vocal") not in ("separate_vocal", "split_stem", "split_stem_advanced"):
            return "type invalid"
        return None
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
        if u.path.startswith("/files/"):
            with LOCK:
                body = UPLOADS.get(u.path[7:])
            if body is None:
                return self.js({"error": "nf"}, 404)
            self.send_response(200)
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if u.path.startswith("/cdn/ext-"):
            # an "extended" song: the uploaded audio followed by 3 s of new material
            with LOCK:
                t = next((t for t in TASKS.values() if t.get("ext_clip") == u.path[9:-4]), None)
                src = UPLOADS.get(t["upload"]) if t else None
            if src is None:
                return self.js({"error": "nf"}, 404)
            body = concat_wav(src, wav_bytes(3.0, freq=550))
            self.send_response(200)
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if u.path.startswith("/cdn/"):
            name = u.path[5:]
            stem_of = None
            if "_" in name and name.endswith(".mp3"):
                clip = name.split("_", 1)[0]
                with LOCK:
                    stem_of = next((t for t in TASKS.values() if clip in t["clip_ids"]), None)
            if stem_of is not None and "#late" in stem_of["prompt"] and "_Vocals" in name:
                # Like the real thing: a long song whose vocal only starts after 20 s of intro
                # (served as WAV bytes; the bridge goes by the file's content, not its name).
                body, ctype = concat_wav(wav_bytes(20.0, freq=0), wav_bytes(8.0, freq=440)), "audio/wav"
                body = concat_wav(body, wav_bytes(12.0, freq=0))
            elif stem_of is not None and stem_of["model"] == "ai-music-api/upload-and-cover-audio" and "_Vocals" in name:
                body, ctype = wav_bytes(30.0, freq=0), "audio/wav"     # an instrumental cover has no vocal
            elif name.endswith(".mp3"):
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
            parent = t.get("parent") or {}
            if parent.get("model") == "ai-music-api/upload-and-extend-audio":
                data["resultJson"] = json.dumps({"resultUrls": [f"{self.base()}/cdn/ext-{t['audio_id']}.wav"]})
            else:
                data["resultJson"] = json.dumps({"resultUrls": [f"{self.base()}/cdn/{t['audio_id']}.wav"]})
        elif t["model"] == "ai-music-api/separate-vocals":
            names = STEMS_12 if t["sep_type"] == "split_stem" else STEMS_2
            data["resultJson"] = json.dumps({"resultUrls": [f"{self.base()}/cdn/{t['audio_id']}_{n.replace(' ', '%20')}.mp3"
                                                            for n in names]})
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

    def upload(self):
        ctype = self.headers.get("Content-Type", "")
        if not ctype.startswith("multipart/form-data; boundary="):
            return self.js({"code": 400, "msg": "multipart/form-data required"})
        boundary = ctype.split("boundary=", 1)[1].encode()
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        fields = {}
        for part in body.split(b"--" + boundary):
            if b"\r\n\r\n" not in part:
                continue
            head, value = part.split(b"\r\n\r\n", 1)
            if value.endswith(b"\r\n"):
                value = value[:-2]
            name = head.split(b'name="', 1)[1].split(b'"', 1)[0].decode() if b'name="' in head else ""
            fields[name] = value
        if not fields.get("uploadPath") or "file" not in fields:
            return self.js({"code": 400, "msg": "file and uploadPath are required"})
        up = fields["uploadPath"].decode()
        if up.startswith("/") or up.endswith("/"):
            return self.js({"code": 400, "msg": "uploadPath must not start or end with /"})
        data = fields["file"]
        if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
            return self.js({"code": 400, "msg": "not a wav"})
        name = fields.get("fileName", b"").decode() or uuid.uuid4().hex + ".wav"
        key = up + "/" + name
        with wave.open(io.BytesIO(data)) as w:
            secs = w.getnframes() / float(w.getframerate())
        with LOCK:
            UPLOADS[key] = data
            UPLOADS_SECONDS[key] = secs
            LOG.append({"model": "upload", "input": {"uploadPath": up, "fileName": name, "bytes": len(data)}})
        return self.js({"success": True, "code": 200, "msg": "File uploaded successfully",
                        "data": {"fileName": name, "filePath": key, "downloadUrl": f"{self.base()}/files/{key}",
                                 "fileSize": len(data), "mimeType": "audio/wav", "uploadedAt": "2026-10-17T00:00:00Z"}})

    def do_POST(self):
        if not self.authed():
            return
        if urlsplit(self.path).path == "/api/file-stream-upload":
            return self.upload()
        if urlsplit(self.path).path != "/api/v1/jobs/createTask":
            return self.js({"code": 404, "msg": "not found"})
        try:
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)))
        except ValueError:
            return self.js({"code": 422, "msg": "invalid json"})
        if set(body) - {"model", "input", "callBackUrl"}:
            return self.js({"code": 422, "msg": "unexpected top-level fields"})
        model, inp = body.get("model"), body.get("input")
        err = validate(model, inp, self.base())
        if err:
            return self.js({"code": 422, "msg": err})
        prompt = str(inp.get("prompt", "") or inp.get("style", ""))
        parent = None
        linked_clip = None
        if model == "ai-music-api/separate-vocals" and "audio_url" in inp:
            # Separation of a link: the stems are named after the linked clip.
            clip = urlsplit(inp["audio_url"]).path.rsplit("/", 1)[-1].rsplit(".", 1)[0]
            with LOCK:
                parent = next((t for t in TASKS.values() if clip in t["clip_ids"]), None)
            if not parent or "#nosep" in parent["prompt"]:
                return self.js({"code": 422, "msg": "audio_url could not be fetched"})
            linked_clip = clip
            prompt = parent["prompt"]
        elif model == "ai-music-api/separate-vocals":
            with LOCK:
                parent = TASKS.get(inp["task_id"])
            if not parent or inp["audio_id"] not in parent["clip_ids"]:
                return self.js({"code": 422, "msg": "task_id/audio_id not found"})
            # What the real Kie.ai answered for a result made from uploaded audio (reported by a user,
            # October 2026): the task id doesn't name a music record it can separate.
            if parent["model"] in UPLOAD_TASK_MODELS:
                return self.js({"code": 400, "msg": "Record does not exist"})
            prompt = parent["prompt"]
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
        clip_ids = [str(uuid.uuid4()), str(uuid.uuid4())]
        with LOCK:
            TASKS[tid] = {"model": model, "prompt": prompt, "polls": 0, "clip_ids": clip_ids,
                          "audio_id": inp.get("audio_id") or linked_clip, "parent": parent, "sep_type": inp.get("type", "separate_vocal"),
                          "upload": (inp.get("upload_url") or "")[len(self.base()) + 7:] or None,
                          "ext_clip": clip_ids[0] if model == "ai-music-api/upload-and-extend-audio" else None}
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
