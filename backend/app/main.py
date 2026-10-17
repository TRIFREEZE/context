"""Context licensing API.

    uvicorn app.main:app --host 127.0.0.1 --port 8787        (run from the backend folder)

Receives ONLY: license key, hashed device ID, plugin version.
Never receives prompts, audio, provider credentials, cookies or session tokens.
There are no admin endpoints: administration is the offline CLI (python -m app.admin).
"""
from __future__ import annotations

import threading
import time
from collections import defaultdict, deque

from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from pydantic import BaseModel, ConfigDict, Field

from . import licenses
from .config import Settings, load_settings
from .crypto import load_private_key, public_key_b64
from .db import connect


class ActivateBody(BaseModel):
    model_config = ConfigDict(extra="forbid")
    license_key: str = Field(min_length=10, max_length=40)
    device_id: str = Field(min_length=32, max_length=32)
    plugin_version: str = Field(min_length=1, max_length=32)


class DeactivateBody(BaseModel):
    model_config = ConfigDict(extra="forbid")
    license_key: str = Field(min_length=10, max_length=40)
    device_id: str = Field(min_length=32, max_length=32)


class RateLimiter:
    """Small in-memory sliding-window limiter per client address (brute-force protection)."""

    def __init__(self, per_minute: int):
        self.per_minute = per_minute
        self.hits: dict[str, deque] = defaultdict(deque)
        self.lock = threading.Lock()

    def allow(self, client: str) -> bool:
        now = time.monotonic()
        with self.lock:
            q = self.hits[client]
            while q and now - q[0] > 60:
                q.popleft()
            if len(q) >= self.per_minute:
                return False
            q.append(now)
            if len(self.hits) > 10000:  # bound memory
                self.hits.clear()
            return True


def error(code: str, message: str, status: int) -> JSONResponse:
    return JSONResponse({"error": code, "message": message}, status_code=status)


def create_app(settings: Settings | None = None) -> FastAPI:
    settings = settings or load_settings()
    signing_key = load_private_key(settings.signing_key_path)
    limiter = RateLimiter(settings.rate_limit_per_minute)
    db_lock = threading.Lock()
    conn = connect(settings.db_path)
    conn_holder = {"conn": conn}

    app = FastAPI(title="Context Licensing", version="1.0", docs_url=None, redoc_url=None, openapi_url=None)
    app.state.settings = settings
    app.state.public_key = public_key_b64(signing_key)

    @app.exception_handler(RequestValidationError)
    async def _bad_request(_request: Request, _exc: RequestValidationError):
        return error("invalid_request", "Request body is malformed.", 400)

    @app.middleware("http")
    async def _rate_limit(request: Request, call_next):
        if request.url.path.startswith("/licenses/"):
            client = request.client.host if request.client else "unknown"
            if not limiter.allow(client):
                return error("rate_limited", "Too many requests. Try again in a minute.", 429)
        return await call_next(request)

    def grant_response(g: licenses.TokenGrant) -> dict:
        return {
            "token": g.token,
            "refresh_after": licenses.iso(g.refresh_after),
            "offline_until": licenses.iso(g.offline_until),
            "license": {
                "status": g.status,
                "expires_at": g.expires_at,
                "max_devices": g.max_devices,
                "activated_devices": g.activated_devices,
            },
        }

    def run(fn):
        try:
            with db_lock:
                return fn(conn_holder["conn"])
        except licenses.LicenseError as e:
            return error(e.code, e.message, e.status)

    @app.get("/health")
    def health():
        return {"ok": True}

    @app.get("/version")
    def version():
        return {
            "latest_version": settings.latest_version,
            "download_url": settings.download_url,
            "release_notes": settings.release_notes,
        }

    @app.post("/licenses/activate")
    def activate(body: ActivateBody):
        def op(c):
            return grant_response(licenses.activate(c, signing_key, settings, body.license_key, body.device_id, body.plugin_version))
        return run(op)

    @app.post("/licenses/validate")
    def validate(body: ActivateBody):
        def op(c):
            return grant_response(licenses.validate(c, signing_key, settings, body.license_key, body.device_id, body.plugin_version))
        return run(op)

    @app.post("/licenses/deactivate")
    def deactivate(body: DeactivateBody):
        def op(c):
            return {"deactivated": licenses.deactivate(c, body.license_key, body.device_id)}
        return run(op)

    return app


def _lazy_app():
    # Only build the real app when imported by uvicorn ("app.main:app").
    return create_app()


class _LazyApp:
    def __init__(self):
        self._app = None

    async def __call__(self, scope, receive, send):
        if self._app is None:
            self._app = _lazy_app()
        await self._app(scope, receive, send)


app = _LazyApp()
