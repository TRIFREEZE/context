"""License business logic: creation, activation, validation, deactivation, revocation."""
from __future__ import annotations

import hashlib
import re
import secrets
import sqlite3
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from .config import Settings
from .crypto import sign_token
from .db import transaction

# Crockford-style alphabet: no I, L, O, U (avoids confusion when typing keys).
ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
KEY_RE = re.compile(r"^CTX-[0-9A-HJKMNP-TV-Z]{4}-[0-9A-HJKMNP-TV-Z]{4}-[0-9A-HJKMNP-TV-Z]{4}-[0-9A-HJKMNP-TV-Z]{4}$")
DEVICE_RE = re.compile(r"^[0-9a-f]{32}$")
VERSION_RE = re.compile(r"^[0-9A-Za-z.+\-]{1,32}$")


class LicenseError(Exception):
    """A definitive refusal. `code` is stable and shown to the plugin; `status` is the HTTP status."""

    def __init__(self, code: str, message: str, status: int):
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status


@dataclass
class TokenGrant:
    token: str
    refresh_after: datetime
    offline_until: datetime
    status: str
    expires_at: str | None
    max_devices: int
    activated_devices: int


def now_utc() -> datetime:
    return datetime.now(timezone.utc).replace(microsecond=0)


def iso(dt: datetime) -> str:
    return dt.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_iso(text: str) -> datetime:
    return datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)


def normalise_key(raw: str) -> str:
    key = (raw or "").strip().upper().replace(" ", "")
    if len(key) == 19 and not key.startswith("CTX-"):  # typed without prefix
        key = "CTX-" + key
    return key


def hash_key(key: str) -> str:
    return hashlib.sha256(("context-license-v1:" + key).encode("utf-8")).hexdigest()


def generate_key() -> str:
    """CTX-XXXX-XXXX-XXXX-XXXX with 80 bits from a CSPRNG."""
    blocks = ["".join(secrets.choice(ALPHABET) for _ in range(4)) for _ in range(4)]
    return "CTX-" + "-".join(blocks)


def validate_inputs(key: str, device_id: str | None = None, plugin_version: str | None = None) -> None:
    if not KEY_RE.match(key):
        raise LicenseError("invalid_key_format", "License keys look like CTX-XXXX-XXXX-XXXX-XXXX.", 400)
    if device_id is not None and not DEVICE_RE.match(device_id):
        raise LicenseError("invalid_device", "Device identifier is malformed.", 400)
    if plugin_version is not None and not VERSION_RE.match(plugin_version):
        raise LicenseError("invalid_version", "Plugin version is malformed.", 400)


# --------------------------------------------------------------------------- admin

def create_license(conn: sqlite3.Connection, max_devices: int, expires_days: int | None = None, note: str = "") -> str:
    if not 1 <= max_devices <= 100:
        raise ValueError("max_devices must be between 1 and 100")
    for _ in range(10):
        key = generate_key()
        try:
            expires = iso(now_utc() + timedelta(days=expires_days)) if expires_days else None
            conn.execute(
                "INSERT INTO licenses (key_hash, key_hint, status, created_at, expires_at, max_devices, note) "
                "VALUES (?, ?, 'active', ?, ?, ?, ?)",
                (hash_key(key), key[-4:], iso(now_utc()), expires, max_devices, note[:200]),
            )
            return key
        except sqlite3.IntegrityError:
            continue  # astronomically unlikely collision: try again
    raise RuntimeError("could not generate a unique license key")


def _find(conn: sqlite3.Connection, key: str) -> sqlite3.Row:
    row = conn.execute("SELECT * FROM licenses WHERE key_hash = ?", (hash_key(key),)).fetchone()
    if row is None:
        raise LicenseError("unknown_license", "This license key was not found.", 404)
    return row


def set_status(conn: sqlite3.Connection, key: str, status: str) -> None:
    key = normalise_key(key)
    validate_inputs(key)
    _find(conn, key)
    conn.execute("UPDATE licenses SET status = ? WHERE key_hash = ?", (status, hash_key(key)))


def reset_activations(conn: sqlite3.Connection, key: str) -> int:
    key = normalise_key(key)
    validate_inputs(key)
    lic = _find(conn, key)
    cur = conn.execute(
        "UPDATE activations SET deactivated_at = ? WHERE license_id = ? AND deactivated_at IS NULL",
        (iso(now_utc()), lic["id"]),
    )
    return cur.rowcount


def list_licenses(conn: sqlite3.Connection) -> list[dict]:
    rows = conn.execute(
        "SELECT l.*, (SELECT COUNT(*) FROM activations a WHERE a.license_id = l.id AND a.deactivated_at IS NULL) AS active_devices "
        "FROM licenses l ORDER BY l.id").fetchall()
    return [dict(r) for r in rows]


def list_activations(conn: sqlite3.Connection, key: str | None = None) -> list[dict]:
    if key:
        key = normalise_key(key)
        validate_inputs(key)
        lic = _find(conn, key)
        rows = conn.execute("SELECT a.*, l.key_hint FROM activations a JOIN licenses l ON l.id = a.license_id "
                            "WHERE a.license_id = ? ORDER BY a.id", (lic["id"],)).fetchall()
    else:
        rows = conn.execute("SELECT a.*, l.key_hint FROM activations a JOIN licenses l ON l.id = a.license_id ORDER BY a.id").fetchall()
    return [dict(r) for r in rows]


# --------------------------------------------------------------------------- plugin-facing

def _check_usable(lic: sqlite3.Row) -> None:
    if lic["status"] == "revoked":
        raise LicenseError("revoked", "This license has been revoked.", 403)
    if lic["expires_at"] and parse_iso(lic["expires_at"]) <= now_utc():
        raise LicenseError("expired", "This license has expired.", 403)


def _active_count(conn: sqlite3.Connection, license_id: int) -> int:
    return conn.execute("SELECT COUNT(*) FROM activations WHERE license_id = ? AND deactivated_at IS NULL",
                        (license_id,)).fetchone()[0]


def _grant(conn, key_private: Ed25519PrivateKey, settings: Settings, lic: sqlite3.Row, device_id: str) -> TokenGrant:
    now = now_utc()
    refresh_after = now + timedelta(hours=settings.refresh_hours)
    offline_until = now + timedelta(days=settings.offline_grace_days)
    if lic["expires_at"]:
        offline_until = min(offline_until, parse_iso(lic["expires_at"]))
        refresh_after = min(refresh_after, offline_until)
    payload = {
        "v": 1,
        "product": "context",
        "license_hint": lic["key_hint"],
        "device_id": device_id,
        "iat": iso(now),
        "refresh_after": iso(refresh_after),
        "offline_until": iso(offline_until),
        "license_expires_at": lic["expires_at"],
        "max_devices": lic["max_devices"],
    }
    return TokenGrant(
        token=sign_token(key_private, payload),
        refresh_after=refresh_after,
        offline_until=offline_until,
        status=lic["status"],
        expires_at=lic["expires_at"],
        max_devices=lic["max_devices"],
        activated_devices=_active_count(conn, lic["id"]),
    )


def activate(conn, key_private, settings: Settings, raw_key: str, device_id: str, plugin_version: str) -> TokenGrant:
    key = normalise_key(raw_key)
    validate_inputs(key, device_id, plugin_version)
    with transaction(conn):
        lic = _find(conn, key)
        _check_usable(lic)
        existing = conn.execute("SELECT * FROM activations WHERE license_id = ? AND device_id = ?",
                                (lic["id"], device_id)).fetchone()
        now = iso(now_utc())
        if existing and existing["deactivated_at"] is None:
            conn.execute("UPDATE activations SET last_seen_at = ?, plugin_version = ? WHERE id = ?",
                         (now, plugin_version, existing["id"]))
        else:
            if _active_count(conn, lic["id"]) >= lic["max_devices"]:
                raise LicenseError("device_limit",
                                   f"This license is already active on {lic['max_devices']} device(s). "
                                   "Deactivate another device first.", 409)
            if existing:
                conn.execute("UPDATE activations SET deactivated_at = NULL, activated_at = ?, last_seen_at = ?, plugin_version = ? "
                             "WHERE id = ?", (now, now, plugin_version, existing["id"]))
            else:
                conn.execute("INSERT INTO activations (license_id, device_id, plugin_version, activated_at, last_seen_at) "
                             "VALUES (?, ?, ?, ?, ?)", (lic["id"], device_id, plugin_version, now, now))
        return _grant(conn, key_private, settings, lic, device_id)


def validate(conn, key_private, settings: Settings, raw_key: str, device_id: str, plugin_version: str) -> TokenGrant:
    key = normalise_key(raw_key)
    validate_inputs(key, device_id, plugin_version)
    with transaction(conn):
        lic = _find(conn, key)
        _check_usable(lic)
        act = conn.execute("SELECT * FROM activations WHERE license_id = ? AND device_id = ? AND deactivated_at IS NULL",
                           (lic["id"], device_id)).fetchone()
        if act is None:
            raise LicenseError("not_activated", "This device is not activated for this license.", 403)
        conn.execute("UPDATE activations SET last_seen_at = ?, plugin_version = ? WHERE id = ?",
                     (iso(now_utc()), plugin_version, act["id"]))
        return _grant(conn, key_private, settings, lic, device_id)


def deactivate(conn, raw_key: str, device_id: str) -> bool:
    key = normalise_key(raw_key)
    validate_inputs(key, device_id)
    with transaction(conn):
        lic = _find(conn, key)
        cur = conn.execute("UPDATE activations SET deactivated_at = ? WHERE license_id = ? AND device_id = ? AND deactivated_at IS NULL",
                           (iso(now_utc()), lic["id"], device_id))
        return cur.rowcount > 0
