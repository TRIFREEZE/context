"""Licensing backend tests: creation, activation, device limits, revocation,
deactivation, token verification, grace windows, input validation, rate limiting."""
from __future__ import annotations

import sys
from dataclasses import replace
from datetime import timedelta
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from app import licenses  # noqa: E402
from app.config import Settings  # noqa: E402
from app.crypto import generate_private_key, public_key_b64, verify_token, sign_token, b64url, b64url_decode  # noqa: E402
from app.db import connect  # noqa: E402
from app.main import create_app  # noqa: E402
from app import admin  # noqa: E402

DEV_A = "a" * 32
DEV_B = "b" * 32
DEV_C = "c" * 32


@pytest.fixture()
def env(tmp_path):
    settings = replace(Settings(), db_path=tmp_path / "db.sqlite3", signing_key_path=tmp_path / "key.pem", rate_limit_per_minute=1000)
    key = generate_private_key(settings.signing_key_path)
    client = TestClient(create_app(settings))
    conn = connect(settings.db_path)
    yield settings, key, client, conn
    conn.close()


def activate(client, key, device=DEV_A, version="0.1.0"):
    return client.post("/licenses/activate", json={"license_key": key, "device_id": device, "plugin_version": version})


def test_key_format_and_entropy():
    keys = {licenses.generate_key() for _ in range(2000)}
    assert len(keys) == 2000
    for k in keys:
        assert licenses.KEY_RE.match(k), k
        assert not set(k[4:].replace("-", "")) & set("ILOU")


def test_create_and_activate_returns_verifiable_token(env):
    settings, key, client, conn = env
    lic = licenses.create_license(conn, 2)
    r = activate(client, lic)
    assert r.status_code == 200, r.text
    body = r.json()
    payload = verify_token(public_key_b64(key), body["token"])
    assert payload is not None
    assert payload["device_id"] == DEV_A
    assert payload["product"] == "context"
    assert payload["license_hint"] == lic[-4:]
    assert lic not in body["token"]  # the full key never appears in the token
    assert body["license"]["activated_devices"] == 1
    off = licenses.parse_iso(payload["offline_until"]) - licenses.parse_iso(payload["iat"])
    assert off == timedelta(days=7)


def test_key_normalisation(env):
    _, _, client, conn = env
    lic = licenses.create_license(conn, 2)
    assert activate(client, "  " + lic.lower() + " ").status_code == 200
    assert activate(client, lic[4:], DEV_B).status_code == 200  # typed without CTX- prefix


def test_device_limit_and_reactivation(env):
    _, _, client, conn = env
    lic = licenses.create_license(conn, 2)
    assert activate(client, lic, DEV_A).status_code == 200
    assert activate(client, lic, DEV_A).status_code == 200  # same device again: no extra slot used
    assert activate(client, lic, DEV_B).status_code == 200
    r = activate(client, lic, DEV_C)
    assert r.status_code == 409 and r.json()["error"] == "device_limit"

    r = client.post("/licenses/deactivate", json={"license_key": lic, "device_id": DEV_A})
    assert r.status_code == 200 and r.json()["deactivated"] is True
    assert activate(client, lic, DEV_C).status_code == 200
    # A has been deactivated, validate must fail for A
    r = client.post("/licenses/validate", json={"license_key": lic, "device_id": DEV_A, "plugin_version": "0.1.0"})
    assert r.status_code == 403 and r.json()["error"] == "not_activated"


def test_revocation_blocks_activation_and_validation(env):
    _, _, client, conn = env
    lic = licenses.create_license(conn, 2)
    assert activate(client, lic).status_code == 200
    licenses.set_status(conn, lic, "revoked")
    r = client.post("/licenses/validate", json={"license_key": lic, "device_id": DEV_A, "plugin_version": "0.1.0"})
    assert r.status_code == 403 and r.json()["error"] == "revoked"
    assert activate(client, lic, DEV_B).json()["error"] == "revoked"
    licenses.set_status(conn, lic, "active")
    assert client.post("/licenses/validate", json={"license_key": lic, "device_id": DEV_A, "plugin_version": "0.1.0"}).status_code == 200


def test_expired_license_and_grace_capped_at_expiry(env):
    _, key, client, conn = env
    lic = licenses.create_license(conn, 2, expires_days=3)
    payload = verify_token(public_key_b64(key), activate(client, lic).json()["token"])
    assert licenses.parse_iso(payload["offline_until"]) <= licenses.parse_iso(payload["license_expires_at"])

    conn.execute("UPDATE licenses SET expires_at = ? WHERE key_hash = ?", ("2000-01-01T00:00:00Z", licenses.hash_key(lic)))
    r = client.post("/licenses/validate", json={"license_key": lic, "device_id": DEV_A, "plugin_version": "0.1.0"})
    assert r.status_code == 403 and r.json()["error"] == "expired"


def test_reset_activations(env):
    _, _, client, conn = env
    lic = licenses.create_license(conn, 1)
    assert activate(client, lic, DEV_A).status_code == 200
    assert activate(client, lic, DEV_B).status_code == 409
    assert licenses.reset_activations(conn, lic) == 1
    assert activate(client, lic, DEV_B).status_code == 200


def test_unknown_and_malformed_inputs(env):
    _, _, client, _ = env
    assert activate(client, "CTX-0000-0000-0000-0000").json()["error"] == "unknown_license"
    assert activate(client, "not a key at all").json()["error"] == "invalid_key_format"
    assert activate(client, "CTX-0000-0000-0000-0000", device="Z" * 32).json()["error"] == "invalid_device"
    assert activate(client, "CTX-0000-0000-0000-0000", version="1.0; DROP TABLE licenses").status_code == 400
    r = client.post("/licenses/activate", json={"license_key": "CTX-0000-0000-0000-0000", "device_id": DEV_A,
                                                "plugin_version": "1", "cookie": "session=abc"})
    assert r.status_code == 400  # unknown fields (e.g. credentials) are rejected, never stored
    assert client.post("/licenses/activate", content=b"{oops").status_code == 400
    assert client.post("/licenses/activate", json={"license_key": "x" * 5000}).status_code == 400


def test_sql_injection_attempt_is_inert(env):
    _, _, client, conn = env
    lic = licenses.create_license(conn, 2)
    evil = "CTX-' OR 1=1 --"
    assert activate(client, evil).status_code == 400
    assert len(licenses.list_licenses(conn)) == 1
    assert activate(client, lic).status_code == 200


def test_tampered_and_foreign_tokens_rejected(env, tmp_path):
    _, key, client, conn = env
    lic = licenses.create_license(conn, 2)
    token = activate(client, lic).json()["token"]
    body, sig = token.split(".")
    payload = b64url_decode(body).replace(DEV_A.encode(), DEV_B.encode())
    assert verify_token(public_key_b64(key), b64url(payload) + "." + sig) is None
    other = generate_private_key(tmp_path / "other.pem")
    assert verify_token(public_key_b64(key), sign_token(other, {"device_id": DEV_A})) is None
    assert verify_token(public_key_b64(key), "garbage") is None


def test_version_endpoint(env):
    _, _, client, _ = env
    r = client.get("/version")
    assert r.status_code == 200 and "latest_version" in r.json()


def test_no_admin_or_docs_endpoints_exposed(env):
    _, _, client, _ = env
    for path in ("/docs", "/openapi.json", "/admin", "/licenses", "/licenses/create"):
        assert client.get(path).status_code in (404, 405)


def test_rate_limit(tmp_path):
    settings = replace(Settings(), db_path=tmp_path / "db.sqlite3", signing_key_path=tmp_path / "key.pem", rate_limit_per_minute=5)
    generate_private_key(settings.signing_key_path)
    client = TestClient(create_app(settings))
    codes = [activate(client, "CTX-0000-0000-0000-0000").status_code for _ in range(8)]
    assert codes.count(429) == 3


def test_admin_cli_roundtrip(tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("CONTEXT_LICENSE_DB", str(tmp_path / "db.sqlite3"))
    monkeypatch.setenv("CONTEXT_SIGNING_KEY_FILE", str(tmp_path / "keys" / "k.pem"))
    monkeypatch.setattr(admin, "PLUGIN_KEY_FILE", tmp_path / "plugin_key.txt")
    assert admin.main(["init-keys"]) == 0
    assert (tmp_path / "plugin_key.txt").read_text().strip().splitlines()[-1]
    assert admin.main(["init-keys"]) == 1  # refuses to overwrite
    capsys.readouterr()
    assert admin.main(["create", "--max-devices", "3", "--note", "tester"]) == 0
    key = capsys.readouterr().out.strip()
    assert licenses.KEY_RE.match(key)
    assert admin.main(["list"]) == 0
    out = capsys.readouterr().out
    assert key[-4:] in out and key[4:8] not in out.replace(key[-4:], "")  # full key never listed
    assert admin.main(["revoke", key]) == 0
    assert admin.main(["reset", key]) == 0
    assert admin.main(["revoke", "CTX-0000-0000-0000-0000"]) == 1


def test_database_stores_hash_not_key(env):
    settings, _, client, conn = env
    lic = licenses.create_license(conn, 2)
    activate(client, lic)
    raw = b"".join(p.read_bytes() for p in settings.db_path.parent.glob(settings.db_path.name + "*"))
    assert len(raw) > 0
    assert lic.encode() not in raw
