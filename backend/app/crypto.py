"""Ed25519 signing keys and license tokens.

Token format (compact, JWT-like but simpler):
    base64url(payload_json) + "." + base64url(ed25519_signature(base64url(payload_json)))

The plugin embeds ONLY the public key and verifies signatures offline.
The private key never leaves this server.
"""
from __future__ import annotations

import base64
import json
import os
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
from cryptography.exceptions import InvalidSignature


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def b64url_decode(text: str) -> bytes:
    pad = "=" * (-len(text) % 4)
    return base64.urlsafe_b64decode(text + pad)


def generate_private_key(path: Path, overwrite: bool = False) -> Ed25519PrivateKey:
    if path.exists() and not overwrite:
        raise FileExistsError(f"{path} already exists (use --force to replace it; existing activations will stop validating)")
    key = Ed25519PrivateKey.generate()
    pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
    path.parent.mkdir(parents=True, exist_ok=True)
    # Create with owner-only permissions where the OS supports it.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(pem)
    return key


def load_private_key(path: Path) -> Ed25519PrivateKey:
    if not path.exists():
        raise FileNotFoundError(
            f"Signing key not found at {path}. Run:  python -m app.admin init-keys   (from the backend folder)")
    key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    if not isinstance(key, Ed25519PrivateKey):
        raise ValueError("Signing key must be an Ed25519 private key")
    return key


def public_key_b64(key: Ed25519PrivateKey | Ed25519PublicKey) -> str:
    pub = key.public_key() if isinstance(key, Ed25519PrivateKey) else key
    raw = pub.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    return base64.b64encode(raw).decode("ascii")


def sign_token(key: Ed25519PrivateKey, payload: dict) -> str:
    body = b64url(json.dumps(payload, separators=(",", ":"), sort_keys=True).encode("utf-8"))
    sig = key.sign(body.encode("ascii"))
    return body + "." + b64url(sig)


def verify_token(public_key_b64_text: str, token: str) -> dict | None:
    """Returns the payload if the signature is valid, else None."""
    try:
        body, sig = token.split(".", 1)
        pub = Ed25519PublicKey.from_public_bytes(base64.b64decode(public_key_b64_text))
        pub.verify(b64url_decode(sig), body.encode("ascii"))
        return json.loads(b64url_decode(body))
    except (ValueError, InvalidSignature, TypeError):
        return None
