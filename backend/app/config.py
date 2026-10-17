"""Configuration from environment variables. No secrets are hard-coded."""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

BACKEND_DIR = Path(__file__).resolve().parent.parent


def _int(name: str, default: int, lo: int, hi: int) -> int:
    try:
        value = int(os.environ.get(name, default))
    except ValueError:
        value = default
    return max(lo, min(hi, value))


@dataclass(frozen=True)
class Settings:
    db_path: Path = field(default_factory=lambda: Path(os.environ.get("CONTEXT_LICENSE_DB", BACKEND_DIR / "data" / "licenses.sqlite3")))
    signing_key_path: Path = field(default_factory=lambda: Path(os.environ.get("CONTEXT_SIGNING_KEY_FILE", BACKEND_DIR / "keys" / "signing_key.pem")))
    refresh_hours: int = field(default_factory=lambda: _int("CONTEXT_REFRESH_HOURS", 24, 1, 24 * 7))
    offline_grace_days: int = field(default_factory=lambda: _int("CONTEXT_OFFLINE_GRACE_DAYS", 7, 1, 60))
    default_max_devices: int = field(default_factory=lambda: _int("CONTEXT_DEFAULT_MAX_DEVICES", 2, 1, 100))
    rate_limit_per_minute: int = field(default_factory=lambda: _int("CONTEXT_RATE_LIMIT_PER_MINUTE", 30, 1, 10000))
    latest_version: str = field(default_factory=lambda: os.environ.get("CONTEXT_LATEST_VERSION", "0.1.0"))
    download_url: str = field(default_factory=lambda: os.environ.get("CONTEXT_DOWNLOAD_URL", ""))
    release_notes: str = field(default_factory=lambda: os.environ.get("CONTEXT_RELEASE_NOTES", ""))


def load_settings() -> Settings:
    return Settings()
