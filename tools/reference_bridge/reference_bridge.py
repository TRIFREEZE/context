#!/usr/bin/env python3
"""
Context reference bridge — a TEST bridge implementing the Context Bridge Protocol v1
(docs/BRIDGE_PROTOCOL.md) with synthetic audio.

It does NOT talk to Suno or any other provider, and needs no credentials.
Use it to check that Context's LocalGenerationBridgeProvider works end to end,
and as a template when writing a real bridge.

    python tools/reference_bridge/reference_bridge.py --port 3000

Standard library only (Python 3.9+).

Fault injection for tests — put one of these words in the prompt:
    #fail         job fails with a message
    #authexpired  job fails with errorCode "auth_expired"
    #malformed    status endpoint returns invalid JSON
    #huge         status endpoint returns a 5 MB JSON document
    #deep         status endpoint returns deeply nested JSON
    #badwav       audio is not a WAV file
    #emptywav     audio is 0 bytes
    #bigfile      audio claims 150 MB and streams zeros
    #expired      audio URL answers 410 Gone
    #vanish       status answers 404 (bridge "forgot" the job)
    #flaky        status answers 500 twice, then works
    #die          status closes the connection without answering
    #remoteurl    audioUrl points at a non-local http host (must be refused)
    #slow         generation takes 8 s
"""

import argparse
import io
import json
import math
import random
import struct
import threading
import time
import uuid
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PROTOCOL = "context-bridge-v1"
TYPES = ["one_shot", "loop", "vocal", "instrument", "percussion", "texture", "fx", "melody"]

JOBS = {}
LOCK = threading.Lock()
MAX_BODY = 8 * 1024 * 1024


def synth_wav(prompt: str, bpm: float, seconds: float = 1.5, rate: int = 44100) -> bytes:
    """A short decaying tone; pitch depends on the prompt so different prompts sound different."""
    freq = 110.0 * (2 ** ((sum(map(ord, prompt)) % 24) / 12.0))
    n = int(seconds * rate)
    frames = bytearray()
    for i in range(n):
        t = i / rate
        s = math.sin(2 * math.pi * freq * t) * math.exp(-t * 3.0) * 0.7
        v = int(max(-1.0, min(1.0, s)) * 32767)
        frames += struct.pack("<hh", v, v)
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))
    return buf.getvalue()


class Handler(BaseHTTPRequestHandler):
    server_version = "ContextReferenceBridge/1.0"
    mode = "normal"

    def log_message(self, fmt, *args):  # keep test output quiet
        if self.server.verbose:
            super().log_message(fmt, *args)

    # ---------------------------------------------------------------- helpers
    def send_json(self, status, obj):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_raw(self, status, body: bytes, ctype="application/octet-stream"):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def read_json(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0 or length > MAX_BODY:
            return None
        try:
            return json.loads(self.rfile.read(length).decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return None

    # ---------------------------------------------------------------- routes
    def do_GET(self):
        path = self.path.split("?", 1)[0]
        mode = self.server.mode

        if path == "/context/v1/health":
            if mode == "wrong-protocol":
                return self.send_json(200, {"protocol": "something-else"})
            return self.send_json(200, {"protocol": PROTOCOL, "name": "Context Reference Bridge", "version": "1.0"})

        if path == "/context/v1/capabilities":
            return self.send_json(200, {"generationTypes": TYPES,
                                        "features": ["variation", "cancellation", "audio_conditioning"],
                                        "maxDurationSeconds": 30})

        if path == "/context/v1/generate":
            if mode == "no-generate":
                return self.send_json(404, {"error": "not found"})
            return self.send_json(405, {"error": "use POST"})

        if path.startswith("/context/v1/jobs/"):
            return self.job_status(path[len("/context/v1/jobs/"):])

        if path.startswith("/files/"):
            return self.audio(path[len("/files/"):].removesuffix(".wav"))

        return self.send_json(404, {"error": "not found"})

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        if path == "/context/v1/generate" and self.server.mode != "no-generate":
            return self.generate()
        if path.startswith("/context/v1/jobs/") and path.endswith("/cancel"):
            job_id = path[len("/context/v1/jobs/"):-len("/cancel")]
            with LOCK:
                job = JOBS.get(job_id)
                if job:
                    job["cancelled"] = True
            return self.send_json(200 if job else 404, {"ok": bool(job)})
        return self.send_json(404, {"error": "not found"})

    # ---------------------------------------------------------------- handlers
    def generate(self):
        body = self.read_json()
        if not isinstance(body, dict) or body.get("protocol") != PROTOCOL:
            return self.send_json(400, {"error": "expected a context-bridge-v1 request", "errorCode": "invalid_request"})
        prompt = str(body.get("prompt", ""))
        if not prompt.strip():
            return self.send_json(400, {"error": "prompt is required", "errorCode": "invalid_request"})

        audio_ctx = body.get("audioContext")
        audio_bytes = len(audio_ctx.get("data", "")) * 3 // 4 if isinstance(audio_ctx, dict) else 0

        job_id = uuid.uuid4().hex
        with LOCK:
            JOBS[job_id] = {
                "prompt": prompt,
                "bpm": float(body.get("bpm") or 120),
                "start": time.time(),
                "duration": 8.0 if "#slow" in prompt else 1.0,
                "cancelled": False,
                "polls": 0,
                "audioContextBytes": audio_bytes,
                "hasContext": isinstance(body.get("context"), dict),
            }
        return self.send_json(202, {"jobId": job_id})

    def job_status(self, job_id):
        with LOCK:
            job = JOBS.get(job_id)
            if job:
                job["polls"] += 1
        if not job:
            return self.send_json(404, {"error": "unknown job"})
        p = job["prompt"]

        if "#vanish" in p:
            return self.send_json(404, {"error": "unknown job"})
        if "#die" in p:
            self.close_connection = True
            self.connection.shutdown(2)
            return
        if "#flaky" in p and job["polls"] <= 2:
            return self.send_json(500, {"error": "temporary failure"})
        if "#malformed" in p:
            return self.send_raw(200, b'{"status": "compl', "application/json")
        if "#huge" in p:
            return self.send_raw(200, b'{"status":"generating","pad":"' + b"x" * (5 * 1024 * 1024) + b'"}', "application/json")
        if "#deep" in p:
            return self.send_raw(200, b"[" * 100000, "application/json")

        base = {"jobId": job_id, "audioContextBytes": job["audioContextBytes"], "hasContext": job["hasContext"]}
        if job["cancelled"]:
            return self.send_json(200, {**base, "status": "cancelled"})
        if "#fail" in p:
            return self.send_json(200, {**base, "status": "failed", "error": "simulated provider failure"})
        if "#authexpired" in p:
            return self.send_json(200, {**base, "status": "failed", "error": "upstream session expired", "errorCode": "auth_expired"})

        elapsed = time.time() - job["start"]
        if elapsed < job["duration"]:
            return self.send_json(200, {**base, "status": "generating", "progress": round(elapsed / job["duration"], 3)})

        url = f"/files/{job_id}.wav"
        if "#remoteurl" in p:
            url = f"http://evil.example.com/files/{job_id}.wav"
        return self.send_json(200, {**base, "status": "complete", "progress": 1.0, "audioUrl": url})

    def audio(self, job_id):
        with LOCK:
            job = JOBS.get(job_id)
        if not job:
            return self.send_json(404, {"error": "unknown file"})
        p = job["prompt"]
        if "#expired" in p:
            return self.send_json(410, {"error": "link expired"})
        if "#badwav" in p:
            return self.send_raw(200, b"this is definitely not a wav file " * 50, "audio/wav")
        if "#emptywav" in p:
            return self.send_raw(200, b"", "audio/wav")
        if "#bigfile" in p:
            self.send_response(200)
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Content-Length", str(150 * 1024 * 1024))
            self.end_headers()
            chunk = b"\0" * 65536
            try:
                for _ in range(150 * 16):
                    self.wfile.write(chunk)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        return self.send_raw(200, synth_wav(p, job["bpm"]), "audio/wav")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1", help="interface to bind (default 127.0.0.1, local only)")
    ap.add_argument("--port", type=int, default=3000)
    ap.add_argument("--mode", default="normal", choices=["normal", "wrong-protocol", "no-generate"])
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    server.mode = args.mode
    server.verbose = args.verbose
    print(f"Context reference bridge ({args.mode}) listening on http://{args.host}:{server.server_address[1]}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    random.seed()
    main()
