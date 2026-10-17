"""SQLite storage. Every query is parameterised."""
from __future__ import annotations

import sqlite3
from contextlib import contextmanager
from pathlib import Path

SCHEMA = """
CREATE TABLE IF NOT EXISTS licenses (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    key_hash      TEXT    NOT NULL UNIQUE,      -- SHA-256 of the normalised license key
    key_hint      TEXT    NOT NULL,             -- last 4 characters, for support/admin listings
    status        TEXT    NOT NULL CHECK (status IN ('active', 'revoked')),
    created_at    TEXT    NOT NULL,
    expires_at    TEXT,                         -- NULL = perpetual
    max_devices   INTEGER NOT NULL CHECK (max_devices >= 1),
    note          TEXT    NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS activations (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    license_id      INTEGER NOT NULL REFERENCES licenses(id) ON DELETE CASCADE,
    device_id       TEXT    NOT NULL,           -- already a salted hash computed by the plugin
    plugin_version  TEXT    NOT NULL,
    activated_at    TEXT    NOT NULL,
    last_seen_at    TEXT    NOT NULL,
    deactivated_at  TEXT,
    UNIQUE (license_id, device_id)
);

CREATE INDEX IF NOT EXISTS idx_activations_license ON activations(license_id);
"""


def connect(path: Path) -> sqlite3.Connection:
    path.parent.mkdir(parents=True, exist_ok=True)
    # check_same_thread=False: FastAPI runs sync endpoints in a thread pool; access is serialised by a lock.
    conn = sqlite3.connect(path, timeout=10, isolation_level=None, check_same_thread=False)  # autocommit; explicit BEGIN for transactions
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    conn.execute("PRAGMA journal_mode = WAL")
    conn.executescript(SCHEMA)
    return conn


@contextmanager
def transaction(conn: sqlite3.Connection):
    conn.execute("BEGIN IMMEDIATE")
    try:
        yield conn
        conn.execute("COMMIT")
    except BaseException:
        conn.execute("ROLLBACK")
        raise
