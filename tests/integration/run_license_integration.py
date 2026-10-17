#!/usr/bin/env python3
"""End-to-end licensing test: real FastAPI backend + the plugin's C++ LicenseManager.

Creates a throwaway signing key and database, starts the backend with uvicorn on a
free port, creates licenses, then runs `ContextTests --license-server=...`.
Exits 77 (CTest "skipped") if the backend's Python dependencies aren't installed.
"""
import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tests-exe", required=True)
    ap.add_argument("--backend", required=True)
    args = ap.parse_args()

    backend = Path(args.backend).resolve()
    sys.path.insert(0, str(backend))
    try:
        import fastapi, uvicorn, cryptography  # noqa: F401
    except ImportError as e:
        print(f"SKIP: backend dependencies not installed ({e}). Run: pip install -r backend/requirements-dev.txt")
        return 77

    from app import licenses
    from app.crypto import generate_private_key, public_key_b64
    from app.db import connect

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        env = dict(os.environ, CONTEXT_LICENSE_DB=str(tmp / "db.sqlite3"), CONTEXT_SIGNING_KEY_FILE=str(tmp / "key.pem"),
                   CONTEXT_LATEST_VERSION="9.9.9", CONTEXT_DOWNLOAD_URL="https://example.com/context")
        key = generate_private_key(tmp / "key.pem")
        conn = connect(tmp / "db.sqlite3")
        lic_ok = licenses.create_license(conn, 2)
        lic_one = licenses.create_license(conn, 1)
        lic_revoke = licenses.create_license(conn, 2)
        conn.close()

        port = free_port()
        proc = subprocess.Popen([sys.executable, "-m", "uvicorn", "app.main:app", "--host", "127.0.0.1", "--port", str(port),
                                 "--log-level", "warning"], cwd=backend, env=env)
        try:
            for _ in range(100):
                try:
                    urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=1)
                    break
                except Exception:
                    if proc.poll() is not None:
                        print("backend exited early")
                        return 1
                    time.sleep(0.1)
            cmd = [args.tests_exe, f"--license-server=http://127.0.0.1:{port}", f"--license-key={lic_ok}",
                   f"--license-key-single={lic_one}", f"--license-key-revoke={lic_revoke}",
                   f"--license-public-key={public_key_b64(key)}", f"--license-db={tmp / 'db.sqlite3'}",
                   f"--backend-dir={backend}", f"--python={sys.executable}"]
            print("Running ContextTests license integration against", f"http://127.0.0.1:{port}", flush=True)
            return subprocess.call(cmd, timeout=300)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    sys.exit(main())
