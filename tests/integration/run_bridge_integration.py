#!/usr/bin/env python3
"""Starts reference bridges on free ports, runs `ContextTests --integration`, stops them.

Used by CTest (and therefore by GitHub Actions). Exit code = test exe exit code.
"""
import argparse
import socket
import subprocess
import sys
import time
import urllib.request


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start_bridge(script, port, mode):
    proc = subprocess.Popen([sys.executable, script, "--port", str(port), "--mode", mode],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    url = f"http://127.0.0.1:{port}/context/v1/health"
    for _ in range(100):
        try:
            with urllib.request.urlopen(url, timeout=1):
                return proc
        except Exception:
            if proc.poll() is not None:
                raise RuntimeError(f"bridge ({mode}) exited early: {proc.stdout.read().decode(errors='replace')}")
            time.sleep(0.1)
    proc.kill()
    raise RuntimeError(f"bridge ({mode}) did not start")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tests-exe", required=True)
    ap.add_argument("--bridge", required=True)
    args = ap.parse_args()

    procs = []
    try:
        normal, wrong, nogen = free_port(), free_port(), free_port()
        procs.append(start_bridge(args.bridge, normal, "normal"))
        procs.append(start_bridge(args.bridge, wrong, "wrong-protocol"))
        procs.append(start_bridge(args.bridge, nogen, "no-generate"))
        closed = free_port()  # nothing listens here -> connection refused

        # JUCE's ArgumentList reads long options in --name=value form.
        cmd = [args.tests_exe, f"--integration=http://127.0.0.1:{normal}",
               f"--wrong-protocol=http://127.0.0.1:{wrong}",
               f"--no-generate=http://127.0.0.1:{nogen}",
               f"--closed=http://127.0.0.1:{closed}"]
        print("Running:", " ".join(cmd), flush=True)
        return subprocess.call(cmd, timeout=540)
    finally:
        for p in procs:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()


if __name__ == "__main__":
    sys.exit(main())
