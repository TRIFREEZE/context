#!/usr/bin/env python3
"""
Context <-> Kie.ai bridge.

Runs on YOUR computer at http://127.0.0.1:3000 and speaks the Context Bridge Protocol v1
(docs/BRIDGE_PROTOCOL.md) towards the Context plugin. For each request it calls the
Kie.ai Suno API (https://docs.kie.ai) with YOUR Kie.ai API key, waits for the result,
turns it into a WAV file and hands it back to Context.

    python tools/kie_bridge/kie_bridge.py              # first run asks for your API key
    python tools/kie_bridge/kie_bridge.py --set-key    # replace the saved key
    python tools/kie_bridge/kie_bridge.py --check      # show remaining credits and exit

What gets generated
    Default            Suno "Sounds" (model ai-music-api/sounds): one-shots, loops, textures, FX.
                       Context's BPM and key are passed along; type "Loop" asks for a seamless loop.
    "#song" in prompt  A full Suno song (model ai-music-api/generate). Instrumental unless the
                       Context type is "Vocal".

    With track context (Context's "Audio ctx" on; your captured 8 bars are uploaded to Kie.ai's
    temporary file storage, deleted by Kie.ai after 24 h):
      Layer   Vocal type: ai-music-api/add-vocals on your 8 bars, then ai-music-api/separate-vocals
              -> only the new vocal, in time and in key with your track.
              Other types: ai-music-api/upload-and-cover-audio in the prompt's style, then
              separate-vocals (split_stem) -> the stem matching your prompt (drums, bass, keys...).
      Restyle ai-music-api/upload-and-cover-audio: your 8 bars re-imagined in the prompt's style.
      Continue ai-music-api/upload-and-extend-audio: what comes after your 8 bars.
    Each Kie.ai step costs credits; a Layer request is two paid steps.

Security
    * The API key is stored in your user profile (%APPDATA%\\Context Kie Bridge\\config.json),
      never in the Context plugin, the project folder, GitHub or the DAW project. It is only
      ever sent to api.kie.ai and is never printed.
    * Listens on 127.0.0.1 only and refuses requests from web pages (Origin header) and
      from foreign host names, so a website can't spend your credits.

Requirements: Python 3.9+. Optional: the "miniaudio" package (decodes MP3 when Kie.ai's WAV
conversion is unavailable); scripts\\run-kie-bridge.bat installs it automatically.
"""

import argparse
import base64
import json
import math
import os
import re
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:  # optional: MP3 -> WAV fallback
    import miniaudio  # type: ignore
except Exception:  # pragma: no cover - depends on the user's machine
    miniaudio = None

VERSION = "1.1.1"
PROTOCOL = "context-bridge-v1"
TYPES = ["one_shot", "loop", "vocal", "instrument", "percussion", "texture", "fx", "melody"]
DEFAULT_API_BASE = "https://api.kie.ai"
# File Upload API host per docs.kie.ai/file-upload-api/quickstart (files are deleted after 24 h).
DEFAULT_UPLOAD_BASE = "https://kieai.redpandaai.co"
USER_AGENT = f"ContextKieBridge/{VERSION}"

MAX_BODY = 32 * 1024 * 1024           # largest request Context sends (base64 audio context included)
MAX_CONTEXT_BYTES = 20 * 1024 * 1024  # decoded audio context WAV
MAX_CONTEXT_SECONDS = 120.0
CONTEXT_MODES = ["layer", "cover", "extend"]
MAX_DOWNLOAD = 100 * 1024 * 1024      # Context refuses audio above 100 MB anyway
MAX_JSON = 4 * 1024 * 1024
MAX_CONCURRENT_JOBS = 3
FILE_RETENTION_SECONDS = 24 * 3600

SOUND_MODELS = ["V6", "V6_MINI", "V6_WILD", "V5_5", "V5"]
MUSIC_MODELS = ["V6", "V6_MINI", "V6_WILD", "V5_5", "V5", "V4_5ALL", "V4_5PLUS", "V4_5", "V4"]
SHARP_KEYS = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
FLAT_TO_SHARP = {"DB": "C#", "EB": "D#", "GB": "F#", "AB": "G#", "BB": "A#", "CB": "B", "FB": "E",
                 "E#": "F", "B#": "C"}

# Context types that carry a tempo / should loop / are tonal.
TEMPO_TYPES = {"loop", "percussion", "melody"}
LOOP_TYPES = {"loop"}
UNPITCHED_TYPES = {"fx", "percussion"}


def log(msg: str) -> None:
    print(time.strftime("[%H:%M:%S] ") + msg, flush=True)


# ============================================================================ configuration

def default_config_dir() -> str:
    if os.name == "nt":
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
        return os.path.join(base, "Context Kie Bridge")
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "context-kie-bridge")


class Config:
    def __init__(self, directory: str):
        self.directory = directory
        self.path = os.path.join(directory, "config.json")
        self.data = {}
        try:
            with open(self.path, "r", encoding="utf-8") as f:
                loaded = json.load(f)
            if isinstance(loaded, dict):
                self.data = loaded
        except FileNotFoundError:
            pass
        except (OSError, ValueError) as e:
            log(f"Warning: could not read {self.path} ({e.__class__.__name__}); starting with defaults.")

    @property
    def api_key(self) -> str:
        return (os.environ.get("KIE_API_KEY") or str(self.data.get("api_key") or "")).strip()

    def get(self, name, default):
        v = self.data.get(name)
        return default if v is None else v

    def save(self) -> None:
        os.makedirs(self.directory, exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.data, f, indent=2)
        if os.name != "nt":
            os.chmod(tmp, 0o600)
        os.replace(tmp, self.path)


def clean_key(raw: str) -> str:
    k = (raw or "").strip().strip('"').strip("'").strip()
    if k.lower().startswith("bearer "):
        k = k[7:].strip()
    return k


def looks_like_key(k: str) -> bool:
    return 8 <= len(k) <= 256 and re.fullmatch(r"[A-Za-z0-9._\-~+/=:]+", k) is not None


def mask(k: str) -> str:
    return "..." + k[-4:] if len(k) >= 8 else "(hidden)"


# ============================================================================ Kie.ai client

class UpstreamError(Exception):
    """An error reported by Kie.ai (or the network). `code` is Kie's numeric code when known."""

    def __init__(self, message: str, code=None, data=None, transient=False):
        super().__init__(message)
        self.code = code
        self.data = data
        self.transient = transient


class KieClient:
    def __init__(self, api_base: str, api_key: str, timeout: float = 30.0, upload_base: str = DEFAULT_UPLOAD_BASE):
        self.api_base = api_base.rstrip("/")
        self.upload_base = upload_base.rstrip("/")
        self.api_key = api_key
        self.timeout = timeout

    def _call(self, method: str, path: str, query=None, body=None, base=None, raw_body=None, content_type=None):
        url = (base or self.api_base) + path
        if query:
            url += "?" + urllib.parse.urlencode(query)
        data = raw_body if raw_body is not None else (json.dumps(body).encode("utf-8") if body is not None else None)
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Authorization", "Bearer " + self.api_key)
        req.add_header("Accept", "application/json")
        req.add_header("User-Agent", USER_AGENT)
        if data is not None:
            req.add_header("Content-Type", content_type or "application/json")
        status = 0
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                status = resp.status
                raw = resp.read(MAX_JSON + 1)
        except urllib.error.HTTPError as e:
            status = e.code
            try:
                raw = e.read(MAX_JSON + 1)
            except Exception:
                raw = b""
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            reason = getattr(e, "reason", e)
            raise UpstreamError(f"Can't reach Kie.ai ({reason})", transient=True) from None

        if len(raw) > MAX_JSON:
            raise UpstreamError("Kie.ai sent an unexpectedly large response")
        try:
            doc = json.loads(raw.decode("utf-8")) if raw else None
        except (ValueError, UnicodeDecodeError):
            doc = None
        if not isinstance(doc, dict):
            transient = status >= 500 or status == 429
            raise UpstreamError(f"Kie.ai answered HTTP {status} without valid JSON", code=status or None,
                                transient=transient)

        # Kie.ai reports most errors with HTTP 200 and a "code" field in the body.
        code = doc.get("code", status)
        try:
            code = int(code)
        except (TypeError, ValueError):
            code = status
        if code != 200 or status >= 400:
            if code == 200:
                code = status
            msg = str(doc.get("msg") or doc.get("message") or f"error {code}")
            raise UpstreamError(msg, code=code, data=doc.get("data"), transient=code in (429, 500, 502, 503, 504))
        return doc.get("data")

    def credits(self):
        return self._call("GET", "/api/v1/chat/credit")

    def create_task(self, model: str, task_input: dict) -> str:
        data = self._call("POST", "/api/v1/jobs/createTask", body={"model": model, "input": task_input})
        task_id = data.get("taskId") if isinstance(data, dict) else None
        if not task_id:
            raise UpstreamError("Kie.ai accepted the request but returned no taskId")
        return str(task_id)

    def upload_file(self, path: str, file_name: str, upload_path: str = "context-bridge") -> str:
        """File Stream Upload (docs.kie.ai/file-upload-api/upload-file-stream): multipart POST with
        `file`, `uploadPath`, `fileName`; returns data.downloadUrl. Kie.ai deletes it after 24 h."""
        boundary = "----ContextBridge" + uuid.uuid4().hex
        with open(path, "rb") as f:
            payload = f.read()
        parts = []
        for name, value in (("uploadPath", upload_path), ("fileName", file_name)):
            parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'.encode())
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="{file_name}"\r\n'
                     f'Content-Type: audio/wav\r\n\r\n'.encode() + payload + b"\r\n")
        parts.append(f"--{boundary}--\r\n".encode())
        old_timeout, self.timeout = self.timeout, max(self.timeout, 120.0)
        try:
            data = self._call("POST", "/api/file-stream-upload", base=self.upload_base, raw_body=b"".join(parts),
                              content_type="multipart/form-data; boundary=" + boundary)
        finally:
            self.timeout = old_timeout
        url = data.get("downloadUrl") if isinstance(data, dict) else None
        if not isinstance(url, str) or not url.startswith(("https://", "http://")):
            raise UpstreamError("Kie.ai accepted the upload but returned no download link")
        return url

    def record_info(self, task_id: str) -> dict:
        data = self._call("GET", "/api/v1/jobs/recordInfo", query={"taskId": task_id})
        if not isinstance(data, dict):
            raise UpstreamError("Kie.ai task details were empty", transient=True)
        return data


# ============================================================================ result parsing

def _maybe_json(v):
    if isinstance(v, str) and v.strip()[:1] in ("{", "["):
        try:
            return json.loads(v)
        except ValueError:
            return None
    return v


def _first(d: dict, *names):
    for n in names:
        v = d.get(n)
        if v not in (None, ""):
            return v
    return None


def extract_clips(info: dict) -> list:
    """Returns [{'id', 'url', 'duration'}] from a recordInfo `data` object.

    The docs describe the result in two shapes (`response.data[]` with snake_case fields for
    Suno tasks, `resultJson.resultUrls[]` for market tasks) and older responses used
    `response.sunoData[]` with camelCase. All of them are accepted."""
    clips = []
    containers = [_maybe_json(info.get("response")), _maybe_json(info.get("resultJson"))]
    for c in containers:
        items = []
        if isinstance(c, list):
            items = c
        elif isinstance(c, dict):
            for key in ("data", "sunoData", "clips", "tracks", "result"):
                if isinstance(c.get(key), list):
                    items = c[key]
                    break
            if not items and isinstance(c.get("resultUrls"), list):
                items = [{"audio_url": u} for u in c["resultUrls"]]
        for it in items:
            if isinstance(it, str):
                it = {"audio_url": it}
            if not isinstance(it, dict):
                continue
            url = _first(it, "audio_url", "audioUrl", "source_audio_url", "sourceAudioUrl")
            if not isinstance(url, str) or not url.startswith(("https://", "http://")):
                continue
            clips.append({
                "id": str(_first(it, "id", "audio_id", "audioId", "clip_id", "clipId") or ""),
                "url": url,
                "duration": _first(it, "duration"),
            })
        if clips:
            break
    return clips


def find_wav_url(obj, depth=0):
    """Finds the WAV link in a convert-to-wav result (resultJson.resultUrls / audio_wav_url / ...)."""
    if depth > 6:
        return None
    obj = _maybe_json(obj)
    found = []

    def walk(o, key, d):
        if d > 6:
            return
        o = _maybe_json(o)
        if isinstance(o, dict):
            for k, v in o.items():
                walk(v, str(k), d + 1)
        elif isinstance(o, list):
            for v in o:
                walk(v, key, d + 1)
        elif isinstance(o, str) and o.startswith(("https://", "http://")):
            score = (2 if "wav" in key.lower() else 0) + (1 if o.lower().split("?")[0].endswith(".wav") else 0)
            if "url" in key.lower() or score:
                found.append((score, o))

    walk(obj, "", depth)
    found.sort(key=lambda t: -t[0])
    return found[0][1] if found else None


STEM_ALIASES = {
    "vocal": "vocal", "vocals": "vocal", "lead vocal": "vocal", "instrumental": "instrumental",
    "backing vocals": "backing_vocals", "backing_vocals": "backing_vocals", "drums": "drums", "drum": "drums",
    "bass": "bass", "guitar": "guitar", "keyboard": "keyboard", "keys": "keyboard", "percussion": "percussion",
    "strings": "strings", "synth": "synth", "fx": "fx", "other": "fx", "brass": "brass", "woodwinds": "woodwinds",
}


def _stem_name(raw: str):
    raw = raw.strip().lower().replace("-", " ").replace("_", " ")
    return STEM_ALIASES.get(raw) or STEM_ALIASES.get(raw.replace(" ", "_"))


def extract_stems(info) -> dict:
    """{stem: url} from a separate-vocals result. The docs give the polling result as
    resultJson.resultUrls with file names ending in the stem ("..._Vocals.mp3") and the callback as
    vocal_removal_info.{vocal_url, drums_url, ...}; both are accepted."""
    stems = {}

    def walk(o, key, d):
        if d > 8:
            return
        o = _maybe_json(o)
        if isinstance(o, dict):
            for k, v in o.items():
                walk(v, str(k), d + 1)
        elif isinstance(o, list):
            for v in o:
                walk(v, key, d + 1)
        elif isinstance(o, str) and o.startswith(("https://", "http://")):
            name = None
            k = key.lower()
            if k.endswith("_url") and k not in ("origin_url", "audio_url", "stream_audio_url", "image_url"):
                name = _stem_name(k[:-4])
            if name is None:
                base = urllib.parse.unquote(urllib.parse.urlsplit(o).path.rsplit("/", 1)[-1])
                m = re.search(r"[_\- ]([A-Za-z][A-Za-z _]*?)\.(mp3|wav|flac|m4a|ogg)$", base)
                if m:
                    name = _stem_name(m.group(1))
            if name and name not in stems:
                stems[name] = o

    walk(info, "", 0)
    return stems


# Which separated stem answers a prompt. Order matters: the first keyword found wins.
STEM_KEYWORDS = [
    ("vocal", ["vocal", "vox", "voice", "singer", "sing", "rap", "choir", "acapella", "adlib", "ad-lib", "chant"]),
    ("drums", ["drum", "kick", "snare", "hat", "hihat", "hi-hat", "clap", "breakbeat"]),
    ("bass", ["808", "bass", "sub"]),
    ("guitar", ["guitar", "riff", "strum"]),
    ("keyboard", ["piano", "keys", "rhodes", "organ", "keyboard", "e-piano", "epiano", "bell", "celesta"]),
    ("strings", ["string", "violin", "cello", "viola", "orchestra", "pizzicato", "harp"]),
    ("brass", ["brass", "horn", "trumpet", "trombone", "tuba"]),
    ("woodwinds", ["flute", "sax", "clarinet", "oboe", "woodwind", "bassoon", "pan flute"]),
    ("synth", ["synth", "pad", "lead", "pluck", "arp", "saw", "chord"]),
    ("percussion", ["percussion", "shaker", "conga", "bongo", "tambourine", "perc"]),
    ("fx", ["fx", "riser", "impact", "sweep", "whoosh", "noise", "texture"]),
]
TYPE_DEFAULT_STEM = {"percussion": "drums", "fx": "fx", "texture": "synth", "vocal": "vocal"}


def wanted_stem(prompt: str, ctype: str):
    text = " " + (prompt or "").lower() + " "
    best = None
    for stem, words in STEM_KEYWORDS:
        for w in words:
            i = text.find(w)
            if i >= 0 and (best is None or i < best[0]):
                best = (i, stem)
    return best[1] if best else TYPE_DEFAULT_STEM.get(ctype)


def vocal_gender(prompt: str):
    t = (prompt or "").lower()
    if re.search(r"\b(female|woman|women|girl|feminine|she)\b", t):
        return "f"
    if re.search(r"\b(male|man|men|guy|boy|masculine|he)\b", t):
        return "m"
    return None


STATE_MAP = {
    # documented jobs/recordInfo states
    "waiting": "queued", "queuing": "queued", "generating": "generating", "success": "complete", "fail": "failed",
    # older Suno-style statuses, accepted just in case
    "pending": "queued", "text_success": "generating", "first_success": "generating",
    "create_task_failed": "failed", "generate_audio_failed": "failed", "callback_exception": "failed",
    "sensitive_word_error": "failed",
}


def task_state(info: dict) -> str:
    raw = str(info.get("state") or info.get("status") or "").strip().lower()
    return STATE_MAP.get(raw, "failed" if ("fail" in raw or "error" in raw) else "generating")


# ============================================================================ audio helpers

def download(url: str, dest: str, allow_http_loopback: bool) -> None:
    parts = urllib.parse.urlsplit(url)
    if parts.scheme != "https" and not (allow_http_loopback and parts.scheme == "http"
                                        and parts.hostname in ("127.0.0.1", "localhost")):
        raise UpstreamError("Kie.ai returned a non-https download link; refusing it")
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=120) as resp, open(dest, "wb") as out:
            total = 0
            while True:
                chunk = resp.read(1 << 16)
                if not chunk:
                    break
                total += len(chunk)
                if total > MAX_DOWNLOAD:
                    raise UpstreamError("The generated file is larger than 100 MB")
                out.write(chunk)
    except urllib.error.HTTPError as e:
        raise UpstreamError(f"Downloading the audio failed (HTTP {e.code})", transient=e.code >= 500) from None
    except (urllib.error.URLError, OSError, TimeoutError) as e:
        raise UpstreamError(f"Downloading the audio failed ({getattr(e, 'reason', e)})", transient=True) from None
    if os.path.getsize(dest) == 0:
        raise UpstreamError("The generated file was empty")


def repair_wav(path: str) -> bool:
    """Checks a WAV file and fixes RIFF/data sizes left as placeholders by streaming encoders.
    Returns False if it isn't a usable PCM/float WAV."""
    size = os.path.getsize(path)
    with open(path, "r+b") as f:
        head = f.read(12)
        if len(head) < 12 or head[:4] != b"RIFF" or head[8:12] != b"WAVE":
            return False
        pos, fmt_ok = 12, False
        while pos + 8 <= size:
            f.seek(pos)
            cid, clen = f.read(4), int.from_bytes(f.read(4), "little")
            if cid == b"fmt ":
                fmt = f.read(min(clen, 40))
                if len(fmt) < 16:
                    return False
                tag = int.from_bytes(fmt[0:2], "little")
                channels = int.from_bytes(fmt[2:4], "little")
                rate = int.from_bytes(fmt[4:8], "little")
                fmt_ok = tag in (1, 3, 0xFFFE) and 1 <= channels <= 8 and 8000 <= rate <= 384000
            elif cid == b"data":
                if not fmt_ok:
                    return False
                actual = size - (pos + 8)
                if actual <= 0:
                    return False
                if clen == 0 or clen > actual:          # placeholder or truncated -> point at what exists
                    f.seek(pos + 4)
                    f.write(min(actual, 0xFFFFFFFF).to_bytes(4, "little"))
                    f.seek(4)
                    f.write(min(size - 8, 0xFFFFFFFF).to_bytes(4, "little"))
                return True
            pos += 8 + clen + (clen & 1)
    return False


def decode_to_wav(src: str, dest: str) -> None:
    if miniaudio is None:
        raise UpstreamError("Kie.ai only provided an MP3 and the MP3 decoder isn't installed. "
                            "Start the bridge with scripts\\run-kie-bridge.bat (installs it), "
                            "or run: python -m pip install miniaudio")
    try:
        snd = miniaudio.decode_file(src, output_format=miniaudio.SampleFormat.SIGNED16)
    except Exception as e:
        raise UpstreamError(f"Couldn't decode the generated audio ({e.__class__.__name__})") from None
    if snd.num_frames <= 0:
        raise UpstreamError("The generated audio was empty")
    import wave
    with wave.open(dest, "wb") as w:
        w.setnchannels(snd.nchannels)
        w.setsampwidth(2)
        w.setframerate(snd.sample_rate)
        pcm = snd.samples
        if sys.byteorder != "little":
            pcm.byteswap()
        w.writeframes(pcm.tobytes())


def wav_info(path: str):
    """(seconds, rate, channels) of a PCM WAV, or None."""
    import wave
    try:
        with wave.open(path, "rb") as w:
            rate, ch, n = w.getframerate(), w.getnchannels(), w.getnframes()
            return (n / float(rate) if rate else 0.0), rate, ch
    except Exception:
        return None


def read_mono16(path: str):
    """(rate, array('h') mono) of a WAV or MP3, or None if it can't be read."""
    import array
    import wave
    try:
        with wave.open(path, "rb") as w:
            if w.getsampwidth() == 2:
                rate, ch = w.getframerate(), w.getnchannels()
                a = array.array("h")
                a.frombytes(w.readframes(w.getnframes()))
                if sys.byteorder != "little":
                    a.byteswap()
                return rate, (a if ch == 1 else array.array("h", a[0::ch]))
    except Exception:
        pass
    if miniaudio is None:
        return None
    try:
        snd = miniaudio.decode_file(path, output_format=miniaudio.SampleFormat.SIGNED16, nchannels=1)
        return snd.sample_rate, snd.samples
    except Exception:
        return None


def envelope(rate: int, mono, hop_seconds=0.02, limit_seconds=None):
    hop = max(1, int(rate * hop_seconds))
    n = len(mono) if limit_seconds is None else min(len(mono), int(limit_seconds * rate))
    env = []
    for start in range(0, n - hop + 1, hop):
        acc = 0
        for i in range(start, start + hop, 4):        # every 4th sample is plenty for an envelope
            v = mono[i]
            acc += v * v
        env.append(math.sqrt(acc / (hop / 4.0)))
    return env


def correlation(a, b) -> float:
    n = min(len(a), len(b))
    if n < 10:
        return 0.0
    a, b = a[:n], b[:n]
    ma, mb = sum(a) / n, sum(b) / n
    num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    da = math.sqrt(sum((x - ma) ** 2 for x in a))
    db = math.sqrt(sum((y - mb) ** 2 for y in b))
    return num / (da * db) if da > 0 and db > 0 else 0.0


def find_echo_of_context(result_path: str, context_path: str, context_seconds: float):
    """If the extended result starts with the uploaded context (Kie.ai may return the whole song
    or only the continuation; the docs don't say), returns the seconds to cut so only the new part
    remains. Returns 0 when the result doesn't start with the context."""
    ctx = read_mono16(context_path)
    res = read_mono16(result_path)
    if not ctx or not res:
        return 0.0
    ce = envelope(ctx[0], ctx[1])
    re_ = envelope(res[0], res[1], limit_seconds=context_seconds + 1.0)
    best, best_lag = 0.0, 0
    for lag in range(0, min(25, max(0, len(re_) - 10))):      # up to 0.5 s offset
        c = correlation(ce, re_[lag:])
        if c > best:
            best, best_lag = c, lag
    if best >= 0.7 and len(res[1]) / float(res[0]) > context_seconds + 2.0:
        return context_seconds + best_lag * 0.02
    return 0.0


def cut_wav_start(path: str, seconds: float) -> bool:
    """Drops the first `seconds` of a 16-bit WAV in place (with a 5 ms fade-in)."""
    import array
    import wave
    try:
        with wave.open(path, "rb") as w:
            rate, ch, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
            if width != 2:
                return False
            w.setpos(min(w.getnframes(), int(seconds * rate)))
            data = array.array("h")
            data.frombytes(w.readframes(w.getnframes()))
        fade = min(len(data) // ch, int(0.005 * rate))
        for f in range(fade):
            g = (f + 1) / float(fade)
            for c in range(ch):
                data[f * ch + c] = int(data[f * ch + c] * g)
        tmp = path + ".cut"
        with wave.open(tmp, "wb") as w:
            w.setnchannels(ch)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(data.tobytes())
        os.replace(tmp, path)
        return True
    except Exception:
        return False


def is_silent(path: str, floor: int = 100) -> bool:
    """True when a WAV/MP3 never gets above about -50 dBFS (a stem with nothing in it)."""
    r = read_mono16(path)
    if not r:
        return False
    mono = r[1]
    for i in range(0, len(mono), 7):
        if abs(mono[i]) > floor:
            return False
    return True


def grid_phase(context_path: str, mix_path: str, bar_seconds: float) -> float:
    """Where the context's bar grid sits in a result that was made on it (0 .. one bar), found by
    matching the loudness envelopes; 0 when the match is too weak to trust."""
    ctx = read_mono16(context_path)
    mix = read_mono16(mix_path)
    if not ctx or not mix or bar_seconds <= 0:
        return 0.0
    ce = envelope(ctx[0], ctx[1])
    me = envelope(mix[0], mix[1], limit_seconds=len(ctx[1]) / float(ctx[0]) + bar_seconds + 0.5)
    best, best_lag = 0.0, 0
    for lag in range(0, max(1, min(int(bar_seconds / 0.02), len(me) - 10))):
        c = correlation(ce, me[lag:])
        if c > best:
            best, best_lag = c, lag
    return best_lag * 0.02 if best >= 0.5 else 0.0


def pick_window(path: str, window_seconds: float, step_seconds: float, phase_seconds: float = 0.0):
    """Start (seconds) of the window that should be kept from a result longer than the context.

    Windows start on the context's grid (phase + k * step). The earliest window with at least 90 %
    of the loudest window's energy wins, so a silent intro is skipped but an early, solid part is
    kept. None when the file can't be read or isn't longer than one window."""
    r = read_mono16(path)
    if not r or window_seconds <= 0 or step_seconds <= 0:
        return None
    rate, mono = r
    total = len(mono) / float(rate)
    if total < window_seconds + step_seconds * 0.5:
        return None
    hop = 0.05
    env = envelope(rate, mono, hop_seconds=hop)
    prefix = [0.0]
    for v in env:
        prefix.append(prefix[-1] + v * v)

    def energy(start):
        a = int(round(start / hop))
        b = min(len(env), a + int(round(window_seconds / hop)))
        return prefix[b] - prefix[a] if b > a else 0.0

    starts, t = [], phase_seconds
    while t + window_seconds <= total + 0.02:
        starts.append(t)
        t += step_seconds
    if not starts:
        return None
    energies = [energy(x) for x in starts]
    best = max(energies)
    if best <= 0:
        return None
    for x, e in zip(starts, energies):
        if e >= 0.9 * best:
            return x
    return starts[0]


def cut_wav_window(path: str, start: float, length: float) -> bool:
    """Keeps [start, start + length) of a 16-bit WAV in place, with 3 ms fades at both ends."""
    import array
    import wave
    try:
        with wave.open(path, "rb") as w:
            rate, ch, width, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
            if width != 2:
                return False
            first = max(0, min(n, int(round(start * rate))))
            count = max(0, min(n - first, int(round(length * rate))))
            w.setpos(first)
            data = array.array("h")
            data.frombytes(w.readframes(count))
        frames = len(data) // ch
        fade = min(frames // 4, int(0.003 * rate))
        for f in range(fade):
            g = (f + 1) / float(fade)
            for c in range(ch):
                data[f * ch + c] = int(data[f * ch + c] * g)
                data[(frames - 1 - f) * ch + c] = int(data[(frames - 1 - f) * ch + c] * g)
        tmp = path + ".cut"
        with wave.open(tmp, "wb") as w:
            w.setnchannels(ch)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(data.tobytes())
        os.replace(tmp, path)
        return True
    except Exception:
        return False


def to_pcm16(path: str) -> bool:
    """Makes sure `path` is a 16-bit WAV (re-decoding 24/32-bit WAVs with miniaudio)."""
    import wave
    try:
        with wave.open(path, "rb") as w:
            if w.getsampwidth() == 2:
                return True
    except Exception:
        pass
    if miniaudio is None:
        return False
    try:
        tmp = path + ".16"
        decode_to_wav(path, tmp)
        os.replace(tmp, path)
        return True
    except Exception:
        return False


# ============================================================================ request mapping

def kie_key(key, mode):
    if not key:
        return None
    k = str(key).strip().upper().replace("♯", "#").replace("♭", "B")
    if len(k) >= 2 and k[1] == "B" and k[:2] in FLAT_TO_SHARP:
        k = FLAT_TO_SHARP[k[:2]]
    k = FLAT_TO_SHARP.get(k, k)
    if k not in SHARP_KEYS:
        return None
    return k + ("m" if str(mode or "").strip().lower().startswith("min") else "")


def shorten(text: str, limit: int) -> str:
    text = re.sub(r"\s+", " ", text).strip()
    if len(text) <= limit:
        return text
    cut = text[:limit]
    space = cut.rfind(" ")
    return (cut[:space] if space > limit * 0.6 else cut).rstrip(" ,.;:-")


def build_task(req: dict, cfg: Config):
    """Maps a Context generate request to (kie_model, input, description)."""
    user_prompt = str(req.get("userPrompt") or "")
    prompt = str(req.get("prompt") or user_prompt)
    wants_song = "#song" in prompt.lower() or "#song" in user_prompt.lower()
    prompt = re.sub(r"#(song|sound)\b", "", prompt, flags=re.IGNORECASE)
    ctype = str(req.get("type") or "one_shot")
    bpm = req.get("bpm")

    if wants_song:
        model = str(cfg.get("music_model", "V6"))
        if model not in MUSIC_MODELS:
            model = "V6"
        instrumental = req.get("instrumental")
        if not isinstance(instrumental, bool):
            instrumental = ctype != "vocal"
        task_input = {
            "custom_mode": False,
            "instrumental": instrumental,
            "model": model,
            "prompt": shorten(prompt, 3000),
        }
        return "ai-music-api/generate", task_input, f"Suno song ({model}{', instrumental' if instrumental else ', vocals'})"

    model = str(cfg.get("sound_model", "V6"))
    if model not in SOUND_MODELS:
        model = "V6"
    task_input = {"prompt": shorten(prompt, 500), "model": model}
    if ctype in LOOP_TYPES:
        task_input["sound_loop"] = True
    if ctype in TEMPO_TYPES and isinstance(bpm, (int, float)) and 1 <= bpm <= 300:
        task_input["sound_tempo"] = int(round(bpm))
    key = kie_key(req.get("key"), req.get("mode"))
    if key and ctype not in UNPITCHED_TYPES:
        task_input["sound_key"] = key
    extras = [f"{task_input['sound_tempo']} BPM" if "sound_tempo" in task_input else "",
              task_input.get("sound_key", ""), "loop" if task_input.get("sound_loop") else ""]
    return "ai-music-api/sounds", task_input, "Suno Sounds (" + ", ".join([model] + [e for e in extras if e]) + ")"


class ContextAudio:
    """The track context Context sent with the request (a WAV file on disk)."""

    def __init__(self, path: str, seconds: float, mode: str, bars=None):
        self.path, self.seconds, self.mode, self.bars = path, seconds, mode, bars


def parse_audio_context(req: dict, work_dir: str, job_id: str):
    """Decodes request.audioContext into a WAV file. Returns ContextAudio, None (not sent) or
    raises UpstreamError with a user-facing message."""
    ac = req.get("audioContext")
    if not isinstance(ac, dict) or not ac.get("data"):
        return None
    if str(ac.get("format", "wav")).lower() != "wav" or str(ac.get("encoding", "base64")).lower() != "base64":
        raise UpstreamError("Unsupported audio context format (expected base64 WAV)")
    data = ac.get("data")
    if not isinstance(data, str) or len(data) > MAX_CONTEXT_BYTES * 4 // 3 + 16:
        raise UpstreamError("The track context is too large to upload")
    try:
        raw = base64.b64decode(data, validate=True)
    except Exception:
        raise UpstreamError("The track context isn't valid base64") from None
    path = os.path.join(work_dir, job_id + "-context.wav")
    with open(path, "wb") as f:
        f.write(raw)
    info = wav_info(path) if repair_wav(path) else None
    problem = None
    if not info or info[0] < 0.5:
        problem = "The track context isn't a usable WAV file"
    elif info[0] > MAX_CONTEXT_SECONDS:
        problem = f"The track context is longer than {int(MAX_CONTEXT_SECONDS)} seconds"
    if problem:
        try:
            os.remove(path)
        except OSError:
            pass
        raise UpstreamError(problem)
    mode = str(req.get("contextMode") or "layer").lower()
    if mode not in CONTEXT_MODES:
        mode = "layer"
    bars = ac.get("bars") if isinstance(ac.get("bars"), (int, float)) else None
    return ContextAudio(path, info[0], mode, bars)


def context_plan(req: dict, cfg: Config, ctx: ContextAudio):
    """The Kie.ai steps for a request with track context: (first_model, first_input, stem, label).
    `stem` is the stem to separate out afterwards (None = keep the whole result)."""
    user_prompt = str(req.get("userPrompt") or "")
    prompt = str(req.get("prompt") or user_prompt)
    prompt = re.sub(r"#(song|sound)\b", "", prompt, flags=re.IGNORECASE).strip()
    user_prompt = re.sub(r"#(song|sound)\b", "", user_prompt, flags=re.IGNORECASE).strip() or prompt
    ctype = str(req.get("type") or "one_shot")
    model = str(cfg.get("music_model", "V6"))
    if model not in ("V6", "V6_MINI", "V6_WILD"):
        model = "V6"
    title = shorten(user_prompt, 80) or "Context"
    style = shorten(prompt, 1000)
    negative = shorten(str(cfg.get("negative_tags", "low quality, muffled, clipping")), 200)
    audio_weight = cfg.get("context_audio_weight", 0.8)
    try:
        audio_weight = round(min(1.0, max(0.0, float(audio_weight))), 2)
    except (TypeError, ValueError):
        audio_weight = 0.8
    gender = vocal_gender(user_prompt)
    duration = int(round(min(360.0, max(10.0, ctx.seconds))))

    # A vocal part is always made with add-vocals, whatever the type ("vocal chops" as a Loop):
    # Kie.ai's cover of a track is instrumental here, so its vocal stem would be silence.
    if ctx.mode == "layer" and (ctype == "vocal" or wanted_stem(user_prompt, ctype) == "vocal"):
        inp = {"upload_url": "", "title": title, "negative_tags": negative, "style": style,
               "model": model, "audio_weight": audio_weight}
        if gender:
            inp["vocal_gender"] = gender
        return "ai-music-api/add-vocals", inp, "vocal", "Add vocals to your track -> vocal stem"

    if ctx.mode == "extend":
        instrumental = ctype != "vocal"
        inp = {"upload_url": "", "model": model, "continue_at": round(max(0.1, ctx.seconds - 0.05), 2),
               "instrumental": instrumental, "style": style, "title": shorten(title, 100),
               "negative_tags": negative, "audio_weight": audio_weight}
        if not instrumental and gender:
            inp["vocal_gender"] = gender
        return "ai-music-api/upload-and-extend-audio", inp, None, "Continue your track"

    instrumental = ctype != "vocal"
    inp = {"upload_url": "", "instrumental": instrumental, "model": model, "style": style, "title": title,
           "negative_tags": negative, "audio_weight": audio_weight, "duration": duration}
    if not instrumental and gender:
        inp["vocal_gender"] = gender
    if ctx.mode == "cover":
        return "ai-music-api/upload-and-cover-audio", inp, None, "Restyle your track"
    stem = wanted_stem(user_prompt, ctype)
    label = "Re-imagine your track -> " + (stem + " stem" if stem else "full result (no instrument named)")
    return "ai-music-api/upload-and-cover-audio", inp, stem, label


# Context maps errorCodes containing auth/rate/quota/invalid/... to its own generic messages and
# shows the bridge's text for any other code. The codes below are chosen so the specific Kie.ai
# message reaches the producer where it's more helpful than the generic one.
ERROR_CODES = {
    401: ("kie_api_key_rejected", "Kie.ai rejected the API key. Run the bridge with --set-key to enter it again."),
    402: ("kie_out_of_credits", "Your Kie.ai account is out of credits."),
    429: ("rate_limited", "Kie.ai is rate-limiting requests. Wait a moment and try again."),
}


class step:
    """Names the pipeline step in a Kie.ai error, so "Record does not exist" says where it happened."""

    def __init__(self, what: str):
        self.what = what

    def __enter__(self):
        return self

    def __exit__(self, kind, e, tb):
        if isinstance(e, UpstreamError) and e.code not in (401, 402, 429) and not str(e).startswith(self.what):
            raise UpstreamError(f"{self.what}: {e}", code=e.code, data=e.data, transient=e.transient) from None
        return False


def to_error(e: UpstreamError):
    code, text = ERROR_CODES.get(e.code or 0, ("provider_error", None))
    msg = text or str(e)
    if text and str(e) and str(e).lower() not in text.lower():
        msg = f"{text} ({e})"
    if "sensitive" in str(e).lower() or "moderation" in str(e).lower() or e.code == 451:
        code = "content_filtered"
    return msg, code


# ============================================================================ jobs

class Job:
    def __init__(self, job_id: str, expected_seconds: float):
        self.id = job_id
        self.status = "queued"
        self.progress = 0.0
        self.error = None
        self.error_code = None
        self.file = None
        self.cancelled = False
        self.created = time.time()
        self.started = time.time()
        self.expected = expected_seconds
        self.lock = threading.Lock()

    def snapshot(self) -> dict:
        with self.lock:
            out = {"jobId": self.id, "status": self.status}
            if self.status in ("queued", "generating"):
                out["progress"] = round(self.progress, 3)
            if self.status == "complete":
                out["progress"] = 1.0
                out["audioUrl"] = f"/files/{self.id}.wav"
            if self.status == "failed":
                out["error"] = self.error or "generation failed"
                if self.error_code:
                    out["errorCode"] = self.error_code
            return out

    def set(self, **kw):
        with self.lock:
            for k, v in kw.items():
                setattr(self, k, v)

    def estimate(self):
        with self.lock:
            if self.status not in ("queued", "generating"):
                return
            frac = (time.time() - self.started) / max(1.0, self.expected)
            self.progress = max(self.progress, min(0.9, 0.05 + 0.85 * frac))


class Cancelled(Exception):
    pass


class Bridge:
    def __init__(self, cfg: Config, client: KieClient, audio_dir: str, allow_http_loopback=False,
                 poll_min=3.0, poll_max=10.0, timeout_seconds=720.0):
        self.cfg = cfg
        self.client = client
        self.audio_dir = audio_dir
        self.allow_http_loopback = allow_http_loopback
        self.poll_min, self.poll_max = poll_min, poll_max
        self.timeout_seconds = timeout_seconds
        self.jobs = {}
        self.lock = threading.Lock()
        os.makedirs(audio_dir, exist_ok=True)
        self.cleanup()

    # -- housekeeping
    def cleanup(self):
        cutoff = time.time() - FILE_RETENTION_SECONDS
        try:
            for name in os.listdir(self.audio_dir):
                p = os.path.join(self.audio_dir, name)
                try:
                    if os.path.isfile(p) and os.path.getmtime(p) < cutoff:
                        os.remove(p)
                except OSError:
                    pass
        except OSError:
            pass
        with self.lock:
            for jid in [j for j, job in self.jobs.items() if job.created < cutoff]:
                del self.jobs[jid]

    def active_jobs(self) -> int:
        with self.lock:
            return sum(1 for j in self.jobs.values() if j.status in ("queued", "generating"))

    def get(self, job_id):
        with self.lock:
            return self.jobs.get(job_id)

    # -- job lifecycle
    def submit(self, req: dict) -> Job:
        """Starts a job. Raises UpstreamError for a request that can't be run (bad audio context)."""
        job_id = uuid.uuid4().hex
        ctx = parse_audio_context(req, self.audio_dir, job_id)
        shown = shorten(str(req.get("userPrompt") or req.get("prompt") or ""), 60)
        if ctx is not None:
            model, task_input, stem, label = context_plan(req, self.cfg, ctx)
            job = Job(job_id, 200.0 if stem else 150.0)
            log(f"New request {job.id[:8]}: \"{shown}\" with {ctx.seconds:.1f} s of track context -> {label}")
            pipeline = lambda tmp: self._pipeline_context(job, model, task_input, stem, ctx, tmp)
            extra = [ctx.path]
        else:
            model, task_input, label = build_task(req, self.cfg)
            job = Job(job_id, 150.0 if model.endswith("/generate") else 45.0)
            log(f"New request {job.id[:8]}: \"{shown}\" -> {label}")
            pipeline = lambda tmp: self._pipeline_plain(job, model, task_input, tmp)
            extra = []
        with self.lock:
            self.jobs[job.id] = job
        threading.Thread(target=self._run, args=(job, pipeline, extra), daemon=True).start()
        return job

    def _check(self, job: Job):
        if job.cancelled:
            raise Cancelled()

    def _sleep(self, job: Job, seconds: float):
        end = time.time() + seconds
        while time.time() < end:
            self._check(job)
            job.estimate()
            time.sleep(min(0.25, max(0.0, end - time.time())))

    def _wait_for_task(self, job: Job, task_id: str, deadline: float, what: str) -> dict:
        interval, failures = self.poll_min, 0
        last_state = None
        while True:
            self._check(job)
            if time.time() > deadline:
                raise UpstreamError(f"Kie.ai didn't finish the {what} in time")
            try:
                info = self.client.record_info(task_id)
                failures = 0
            except UpstreamError as e:
                if not e.transient or failures >= 10:
                    raise
                failures += 1
                self._sleep(job, min(30.0, interval * (failures + 1)))
                continue
            state = task_state(info)
            if state != last_state:
                log(f"  {job.id[:8]}: {what} {state}")
                last_state = state
            if state == "complete":
                return info
            if state == "failed":
                msg = str(info.get("failMsg") or info.get("errorMessage") or f"Kie.ai reported the {what} failed")
                code = info.get("failCode") or info.get("errorCode")
                try:
                    code = int(code)
                except (TypeError, ValueError):
                    code = None
                raise UpstreamError(msg, code=code)
            job.set(status="generating" if state == "generating" else "queued")
            p = info.get("progress")
            if isinstance(p, (int, float)) and 0 < p <= 100:
                job.set(progress=max(job.progress, min(0.9, (p / 100.0 if p > 1 else p) * 0.9)))
            self._sleep(job, interval)
            interval = min(self.poll_max, interval * 1.3)

    def _separate(self, job: Job, task_id: str, clip: dict, kind: str, deadline: float) -> dict:
        """Stem separation of a generated clip (docs.kie.ai/suno-api/separate-vocals).

        Kie.ai separates by task_id + audio_id, or by audio_url. Results made from uploaded audio
        (add-vocals, upload-and-cover) are refused by id with "Record does not exist", so the clip's
        own link is used when the id route is refused or there is no clip id. A refused request
        creates no task, so trying the second route can't charge twice."""
        routes = []
        if clip.get("id"):
            routes.append(("task id", {"task_id": task_id, "audio_id": clip["id"], "type": kind}))
        if clip.get("url"):
            routes.append(("audio link", {"audio_url": clip["url"], "type": kind}))
        if not routes:
            raise UpstreamError("Kie.ai returned no clip to separate")
        last = None
        for i, (how, task_input) in enumerate(routes):
            try:
                sep_task = self._create(job, "ai-music-api/separate-vocals", task_input)
            except UpstreamError as e:
                if e.code in (401, 402) or i == len(routes) - 1:
                    raise UpstreamError(f"Kie.ai stem separation: {e}", code=e.code, data=e.data,
                                        transient=e.transient) from None
                log(f"  {job.id[:8]}: stem separation by {how} refused ({e}); trying the {routes[i + 1][0]}")
                last = e
                continue
            with step("Kie.ai stem separation"):
                return self._wait_for_task(job, sep_task, deadline, "stem separation")
        raise last

    def _create(self, job: Job, model: str, task_input: dict) -> str:
        attempt = 0
        while True:
            try:
                return self.client.create_task(model, task_input)
            except UpstreamError as e:
                # Only retry when Kie.ai clearly did not accept the task (rate limit), so a
                # retry can't create a second, paid generation.
                if e.code == 429 and attempt < 3:
                    attempt += 1
                    self._sleep(job, 5.0 * attempt)
                    continue
                raise

    def _convert_to_wav(self, job: Job, task_id: str, clip_id: str, deadline: float):
        try:
            wav_task = self.client.create_task("ai-music-api/convert-to-wav-format",
                                               {"task_id": task_id, "audio_id": clip_id})
        except UpstreamError as e:
            # Already converted earlier: Kie answers 422 and includes the existing link.
            url = find_wav_url(e.data) if e.code == 422 else None
            if url:
                return url
            raise
        info = self._wait_for_task(job, wav_task, deadline, "WAV conversion")
        url = find_wav_url({"r": info.get("resultJson"), "s": info.get("response")})
        if not url:
            raise UpstreamError("WAV conversion finished without a link")
        return url

    def _fetch_audio(self, job: Job, url: str, final: str, tmp_paths: list) -> None:
        """Downloads `url` (WAV or MP3) and leaves a usable WAV at `final`."""
        self._check(job)
        src = final + ".src"
        tmp_paths.append(src)
        download(url, src, self.allow_http_loopback)
        with open(src, "rb") as f:
            magic = f.read(12)
        if magic[:4] == b"RIFF" and magic[8:12] == b"WAVE" and repair_wav(src):
            os.replace(src, final)
        else:
            part = final + ".part"
            tmp_paths.append(part)
            decode_to_wav(src, part)
            os.replace(part, final)

    def _deliver_clip(self, job: Job, task_id: str, clip: dict, deadline: float, final: str, tmp_paths: list):
        """Best-quality WAV of a generated Suno clip: Kie.ai's WAV conversion, else the decoded MP3."""
        if self.cfg.get("convert_to_wav", True) and clip["id"]:
            try:
                wav_url = self._convert_to_wav(job, task_id, clip["id"], deadline)
                self._check(job)
                part = final + ".part"
                tmp_paths.append(part)
                download(wav_url, part, self.allow_http_loopback)
                if repair_wav(part):
                    os.replace(part, final)
                    return
                log(f"  {job.id[:8]}: WAV from Kie.ai wasn't usable, decoding the MP3 instead")
            except Cancelled:
                raise
            except UpstreamError as e:
                log(f"  {job.id[:8]}: WAV conversion unavailable ({e}); using the MP3 instead")
        self._fetch_audio(job, clip["url"], final, tmp_paths)

    def _generate(self, job: Job, model: str, task_input: dict, deadline: float, what: str):
        task_id = self._create(job, model, task_input)
        job.set(status="generating", progress=max(job.progress, 0.05))
        info = self._wait_for_task(job, task_id, deadline, what)
        clips = extract_clips(info)
        if not clips:
            raise UpstreamError("Kie.ai finished but returned no audio")
        return task_id, clips[0]

    # -- pipelines (each returns the path of the finished WAV)
    def _pipeline_plain(self, job: Job, model: str, task_input: dict, tmp_paths: list) -> str:
        deadline = time.time() + self.timeout_seconds
        task_id, clip = self._generate(job, model, task_input, deadline, "generation")
        job.set(progress=0.92)
        final = os.path.join(self.audio_dir, job.id + ".wav")
        self._deliver_clip(job, task_id, clip, deadline, final, tmp_paths)
        return final

    def _pipeline_context(self, job: Job, model: str, task_input: dict, stem, ctx: ContextAudio,
                          tmp_paths: list) -> str:
        deadline = time.time() + self.timeout_seconds + (240.0 if stem else 0.0)
        job.set(status="generating", progress=0.02)
        log(f"  {job.id[:8]}: uploading {ctx.seconds:.1f} s of track context to Kie.ai file storage")
        with step("Uploading your track to Kie.ai"):
            upload_url = self.client.upload_file(ctx.path, job.id + ".wav")
        self._check(job)
        task_input = dict(task_input, upload_url=upload_url)
        job.set(expected=150.0)
        name = model.rsplit("/", 1)[-1]
        with step(f"Kie.ai {name}"):
            task_id, clip = self._generate(job, model, task_input, deadline, name)
        final = os.path.join(self.audio_dir, job.id + ".wav")

        if stem:
            job.set(progress=0.6, started=time.time(), expected=90.0)
            kind = "separate_vocal" if stem in ("vocal", "instrumental") else "split_stem"
            try:
                info = self._separate(job, task_id, clip, kind, deadline)
            except Cancelled:
                raise
            except UpstreamError as e:
                if e.code in (401, 402):
                    raise
                # The generation is already paid for: hand it over rather than failing.
                log(f"  {job.id[:8]}: Kie.ai couldn't separate the {stem} ({e}); returning the full result "
                    f"(it also contains your track)")
                info = None
            if info is not None:
                stems = extract_stems({"r": info.get("resultJson"), "s": info.get("response"), "d": info})
                if stem in stems:
                    log(f"  {job.id[:8]}: keeping the {stem} stem (of {', '.join(sorted(stems))})")
                    job.set(progress=0.93)
                    self._fetch_audio(job, stems[stem], final, tmp_paths)
                    if is_silent(final):
                        log(f"  {job.id[:8]}: the {stem} stem is silent (Kie.ai put no {stem} in it); "
                            f"returning the full result instead")
                    else:
                        self._keep_part_with_sound(job, final, clip, ctx, tmp_paths, stem)
                        return final
                else:
                    log(f"  {job.id[:8]}: no {stem} stem in the separation result ({', '.join(sorted(stems)) or 'none'}); "
                        f"returning the full result")

        job.set(progress=0.92)
        self._deliver_clip(job, task_id, clip, deadline, final, tmp_paths)
        if ctx.mode == "extend":
            cut = find_echo_of_context(final, ctx.path, ctx.seconds)
            if cut > 0 and to_pcm16(final) and cut_wav_start(final, cut):
                log(f"  {job.id[:8]}: the result repeats your track first; cut {cut:.2f} s so only the new part remains")
        return final

    def _keep_part_with_sound(self, job: Job, final: str, clip: dict, ctx: "ContextAudio", tmp_paths: list, what: str):
        """Kie.ai often returns far more than the uploaded bars (a 78 s song for 8 bars) and the new
        part may only start later, so keeping the first bars can give silence. Keep one context-length
        window on the context's bar grid: the first one with real sound in it."""
        info = wav_info(final)
        if not info or not to_pcm16(final):
            return
        total = info[0]
        if total < ctx.seconds + 0.5:
            return
        bars = ctx.bars if isinstance(ctx.bars, int) and ctx.bars > 0 else None
        step = ctx.seconds / bars if bars else ctx.seconds / 4.0
        phase = 0.0
        if clip.get("url"):
            mix = final + ".mix"
            tmp_paths.append(mix)
            try:
                download(clip["url"], mix, self.allow_http_loopback)
                phase = grid_phase(ctx.path, mix, step)
            except Exception:
                phase = 0.0
        start = pick_window(final, ctx.seconds, step, phase)
        if start is None:
            return
        if cut_wav_window(final, start, ctx.seconds):
            where = (f"bars {int(round((start - phase) / step)) + 1}-{int(round((start - phase) / step)) + bars}"
                     if bars else f"{start:.1f}-{start + ctx.seconds:.1f} s")
            log(f"  {job.id[:8]}: Kie.ai returned {total:.1f} s for your {ctx.seconds:.1f} s; kept the {what} from "
                f"{where} (in time with your track)")

    def _run(self, job: Job, pipeline, extra_tmp: list):
        tmp_paths = list(extra_tmp)
        try:
            final = pipeline(tmp_paths)
            self._check(job)
            job.set(status="complete", progress=1.0, file=final)
            log(f"  {job.id[:8]}: ready ({os.path.getsize(final) // 1024} KB WAV)")
        except Cancelled:
            job.set(status="cancelled")
            log(f"  {job.id[:8]}: cancelled (credits already used by Kie.ai are not refunded)")
        except UpstreamError as e:
            msg, code = to_error(e)
            job.set(status="failed", error=msg, error_code=code)
            log(f"  {job.id[:8]}: FAILED - {msg}")
        except Exception as e:  # never let a worker crash silently
            job.set(status="failed", error=f"bridge error: {e.__class__.__name__}", error_code="provider_error")
            log(f"  {job.id[:8]}: FAILED - unexpected {e.__class__.__name__}: {e}")
        finally:
            for p in tmp_paths:
                try:
                    if os.path.exists(p):
                        os.remove(p)
                except OSError:
                    pass


# ============================================================================ HTTP server

LOCAL_HOSTS = {"127.0.0.1", "localhost", "[::1]", "::1"}
JOB_ID_RE = re.compile(r"^[0-9a-f]{32}$")


class Handler(BaseHTTPRequestHandler):
    server_version = f"ContextKieBridge/{VERSION}"

    def log_message(self, fmt, *args):
        if self.server.verbose:
            super().log_message(fmt, *args)

    @property
    def bridge(self) -> Bridge:
        return self.server.bridge

    def send_json(self, status, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def guard(self) -> bool:
        """Only Context on this computer may use the bridge: no web pages, no foreign host names."""
        host = (self.headers.get("Host") or "").strip().lower()
        hostname = host.rsplit(":", 1)[0] if not host.startswith("[") else host.split("]")[0] + "]"
        if hostname not in LOCAL_HOSTS:
            self.send_json(403, {"error": "the bridge only answers requests addressed to 127.0.0.1",
                                 "errorCode": "forbidden"})
            return False
        if self.headers.get("Origin") or self.headers.get("Sec-Fetch-Site") not in (None, "none"):
            self.send_json(403, {"error": "requests from web pages are not allowed", "errorCode": "forbidden"})
            return False
        return True

    def read_json(self):
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            return None
        if length <= 0 or length > MAX_BODY:
            return None
        try:
            return json.loads(self.rfile.read(length).decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return None

    def do_OPTIONS(self):
        self.send_json(403, {"error": "requests from web pages are not allowed", "errorCode": "forbidden"})

    def do_GET(self):
        if not self.guard():
            return
        path = self.path.split("?", 1)[0]
        if path == "/context/v1/health":
            return self.send_json(200, {"protocol": PROTOCOL, "name": "Kie.ai Suno Bridge", "version": VERSION})
        if path == "/context/v1/capabilities":
            return self.send_json(200, {"generationTypes": TYPES,
                                        "features": ["cancellation", "audio_conditioning", "cover", "continuation"],
                                        "contextModes": CONTEXT_MODES,
                                        "maxAudioContextSeconds": int(MAX_CONTEXT_SECONDS),
                                        "maxDurationSeconds": 360})
        if path == "/context/v1/generate":
            return self.send_json(405, {"error": "use POST"})
        if path.startswith("/context/v1/jobs/"):
            job = self.bridge.get(path[len("/context/v1/jobs/"):])
            if not job:
                return self.send_json(404, {"error": "unknown job (was the bridge restarted?)"})
            return self.send_json(200, job.snapshot())
        if path.startswith("/files/") and path.endswith(".wav"):
            return self.send_file(path[len("/files/"):-len(".wav")])
        return self.send_json(404, {"error": "not found"})

    def do_POST(self):
        if not self.guard():
            return
        path = self.path.split("?", 1)[0]
        if path == "/context/v1/generate":
            return self.generate()
        if path.startswith("/context/v1/jobs/") and path.endswith("/cancel"):
            job = self.bridge.get(path[len("/context/v1/jobs/"):-len("/cancel")])
            if job:
                job.set(cancelled=True)
            return self.send_json(200 if job else 404, {"ok": bool(job)})
        return self.send_json(404, {"error": "not found"})

    def generate(self):
        if not (self.headers.get("Content-Type") or "").lower().startswith("application/json"):
            return self.send_json(415, {"error": "expected application/json", "errorCode": "invalid_request"})
        body = self.read_json()
        if not isinstance(body, dict) or body.get("protocol") != PROTOCOL:
            return self.send_json(400, {"error": "expected a context-bridge-v1 request", "errorCode": "invalid_request"})
        if not str(body.get("prompt") or body.get("userPrompt") or "").strip():
            return self.send_json(400, {"error": "prompt is required", "errorCode": "invalid_request"})
        if self.bridge.active_jobs() >= MAX_CONCURRENT_JOBS:
            return self.send_json(429, {"error": "the bridge is already running several generations",
                                        "errorCode": "busy"})
        try:
            job = self.bridge.submit(body)
        except UpstreamError as e:
            return self.send_json(400, {"error": str(e), "errorCode": "audio_context_rejected"})
        return self.send_json(202, {"jobId": job.id})

    def send_file(self, job_id):
        job = self.bridge.get(job_id) if JOB_ID_RE.match(job_id or "") else None
        path = job.file if job and job.status == "complete" else None
        if not path or not os.path.isfile(path):
            return self.send_json(404, {"error": "unknown file"})
        size = os.path.getsize(path)
        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(size))
        self.end_headers()
        with open(path, "rb") as f:
            while True:
                chunk = f.read(1 << 16)
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    return


# ============================================================================ startup

def ask_for_key(cfg: Config) -> bool:
    if not sys.stdin or not sys.stdin.isatty():
        return False
    print()
    print("Paste your Kie.ai API key and press Enter.")
    print("(Find it in your Kie.ai dashboard under API Key. It is saved only on this computer, in")
    print(f" {cfg.path} )")
    for _ in range(3):
        try:
            k = clean_key(input("API key: "))
        except (EOFError, KeyboardInterrupt):
            return False
        if looks_like_key(k):
            cfg.data["api_key"] = k
            cfg.save()
            print(f"Saved key {mask(k)}.")
            return True
        print("That doesn't look like an API key (letters/numbers, no spaces). Try again.")
    return False


def check_key(client: KieClient) -> str:
    """Returns 'ok', 'invalid' or 'unknown' (network problem)."""
    try:
        credits = client.credits()
        log(f"Kie.ai key OK - {credits} credits remaining.")
        return "ok"
    except UpstreamError as e:
        if e.code == 401:
            log("Kie.ai rejected this API key.")
            return "invalid"
        log(f"Couldn't check the key right now ({e}). Continuing anyway.")
        return "unknown"


def main():
    if hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(errors="replace")
        except Exception:
            pass
    ap = argparse.ArgumentParser(description="Context <-> Kie.ai Suno bridge",
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--port", type=int, default=3000)
    ap.add_argument("--set-key", action="store_true", help="enter a new Kie.ai API key")
    ap.add_argument("--check", action="store_true", help="check the key, show credits, exit")
    ap.add_argument("--config-dir", default=default_config_dir(), help=argparse.SUPPRESS)
    ap.add_argument("--api-base", default=DEFAULT_API_BASE, help=argparse.SUPPRESS)  # tests only
    ap.add_argument("--upload-base", default=None, help=argparse.SUPPRESS)              # tests only
    ap.add_argument("--fast-poll", action="store_true", help=argparse.SUPPRESS)       # tests only
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    api = urllib.parse.urlsplit(args.api_base)
    test_upstream = api.scheme == "http" and api.hostname in ("127.0.0.1", "localhost")
    if api.scheme != "https" and not test_upstream:
        print("--api-base must be https", file=sys.stderr)
        return 2

    upload_base = args.upload_base or (args.api_base if test_upstream else DEFAULT_UPLOAD_BASE)
    up = urllib.parse.urlsplit(upload_base)
    if up.scheme != "https" and not (test_upstream and up.hostname in ("127.0.0.1", "localhost")):
        print("--upload-base must be https", file=sys.stderr)
        return 2

    cfg = Config(args.config_dir)
    if args.set_key or not cfg.api_key:
        if not ask_for_key(cfg):
            print("No Kie.ai API key configured. Run this in a console window (scripts\\run-kie-bridge.bat) "
                  "or set the KIE_API_KEY environment variable.", file=sys.stderr)
            return 2

    client = KieClient(args.api_base, cfg.api_key, upload_base=upload_base)
    status = check_key(client)
    while status == "invalid" and not os.environ.get("KIE_API_KEY"):
        if not ask_for_key(cfg):
            return 2
        client = KieClient(args.api_base, cfg.api_key, upload_base=upload_base)
        status = check_key(client)
    if args.check:
        return 0 if status == "ok" else 1

    audio_dir = os.path.join(tempfile.gettempdir(), "context-kie-bridge")
    bridge = Bridge(cfg, client, audio_dir, allow_http_loopback=test_upstream,
                    poll_min=0.2 if args.fast_poll else 3.0, poll_max=0.5 if args.fast_poll else 10.0)

    try:
        server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    except OSError as e:
        print(f"Can't listen on port {args.port} ({e}). Is another bridge (or the reference bridge) "
              f"already running? Close it, or use --port.", file=sys.stderr)
        return 1
    server.daemon_threads = True
    server.bridge = bridge
    server.verbose = args.verbose

    def housekeeping():
        while True:
            time.sleep(3600)
            bridge.cleanup()

    threading.Thread(target=housekeeping, daemon=True).start()
    mp3_note = "" if miniaudio else "  (MP3 decoder not installed: WAV conversion must succeed)"
    log(f"Kie.ai bridge listening on http://127.0.0.1:{server.server_address[1]}{mp3_note}")
    log("In Context: Settings -> Generation -> Provider: Local Generation Bridge, address "
        f"http://127.0.0.1:{server.server_address[1]}, then Test Connection. Leave this window open.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
