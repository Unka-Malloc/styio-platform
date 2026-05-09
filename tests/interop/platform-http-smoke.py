#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path
from urllib.error import URLError
from urllib.request import Request, urlopen


def reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def load_http_capability(binary: str) -> dict[str, object]:
    proc = subprocess.run([binary, "--check-config"], text=True, capture_output=True, check=True)
    return json.loads(proc.stdout)["http_adapter"]


def run_one_request(binary: str, path: str, *, method: str = "GET", body: dict[str, object] | None = None) -> dict[str, object]:
    port = reserve_port()
    env = os.environ.copy()
    env.update(
        {
            "STYIO_PLATFORM_BIND_HOST": "127.0.0.1",
            "STYIO_PLATFORM_BIND_PORT": str(port),
            "STYIO_PLATFORM_POSTGRES_DSN": "postgres://platform@localhost/styio",
            "STYIO_PLATFORM_OBJECT_STORE_PROVIDER": "memory",
            "STYIO_PLATFORM_MTLS_REQUIRED": "true",
        }
    )

    data = None if body is None else json.dumps(body).encode("utf-8")
    proc = subprocess.Popen(
        [binary, "--serve-once"],
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    payload: dict[str, object] | None = None
    try:
        url = f"http://127.0.0.1:{port}{path}"
        request = Request(
            url,
            data=data,
            method=method,
            headers={
                "Content-Type": "application/json",
                "X-Styio-Mtls-Uri-San": "spiffe://styio-platform/tenant/tenant-smoke/role/operator/node/operator-01",
            },
        )
        for _ in range(50):
            if proc.poll() is not None:
                stdout, stderr = proc.communicate()
                print(stdout, file=sys.stdout)
                print(stderr, file=sys.stderr)
                raise AssertionError(f"server exited early with code {proc.returncode}")
            try:
                with urlopen(request, timeout=1) as response:
                    payload = json.loads(response.read().decode("utf-8"))
                break
            except URLError:
                time.sleep(0.1)
        if payload is None:
            raise AssertionError("server did not accept a smoke request")
    finally:
        if proc.poll() is None and payload is None:
            proc.terminate()

    stdout, stderr = proc.communicate(timeout=5)
    if proc.returncode != 0:
        print(stdout, file=sys.stdout)
        print(stderr, file=sys.stderr)
        raise AssertionError(f"server exited with code {proc.returncode}")
    return payload


def main() -> int:
    binary = os.environ.get("STYIO_PLATFORMD_BIN")
    if not binary:
        print("STYIO_PLATFORMD_BIN is required", file=sys.stderr)
        return 2
    if not Path(binary).exists():
        print(f"styio-platformd not found: {binary}", file=sys.stderr)
        return 2

    capability = load_http_capability(binary)
    assert capability.get("fallback_listener_available"), capability

    payload = run_one_request(binary, "/api/styio-platform/v1/health")

    assert payload["returncode"] == 0, payload
    assert payload["payload"]["service"] == "styio-platformd", payload
    assert payload["payload"]["status"] == "ready", payload

    submit = run_one_request(
        binary,
        "/api/styio-platform/v1/jobs",
        method="POST",
        body={
            "tenant_id": "tenant-smoke",
            "user_id": "user-smoke",
            "workspace_id": "workspace-smoke",
            "action": "build",
            "preferred_worker_pool": "linux/x86_64/build/nightly/minimal",
            "job_request": {
                "schema_version": 1,
                "api_path": "/api/styio-platform/v1/jobs",
                "action": "build",
                "manifest_path": "spio.toml",
                "source": {"origin": "file:///tmp/styio-platform-http-smoke"},
                "toolchain": {},
                "workflow": {},
                "target": {},
                "cloud": {},
            },
        },
    )
    assert submit["returncode"] == 0, submit
    assert submit["payload"]["status"] == "queued", submit
    assert submit["payload"]["tenant_id"] == "tenant-smoke", submit
    print("platform HTTP smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
