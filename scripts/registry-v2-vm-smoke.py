#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import sys
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


BASE_PATH = "/api/pafio-registry-control/v1"


def _ustar_octal(value: int, width: int) -> bytes:
    encoded = f"{value:0{width - 1}o}".encode("ascii") + b"\0"
    if len(encoded) != width:
        raise ValueError("ustar fixture numeric field overflow")
    return encoded


def _deterministic_pafio_ustar(files: tuple[tuple[str, bytes], ...]) -> bytes:
    archive = bytearray()
    for path, content in files:
        name = path.encode("utf-8")
        if len(name) > 100:
            raise ValueError("VM smoke fixture path exceeds the ustar name field")
        header = bytearray(512)
        header[0 : len(name)] = name
        header[100:108] = _ustar_octal(0o644, 8)
        header[108:116] = _ustar_octal(0, 8)
        header[116:124] = _ustar_octal(0, 8)
        header[124:136] = _ustar_octal(len(content), 12)
        header[136:148] = _ustar_octal(0, 12)
        header[148:156] = b"        "
        header[156:157] = b"0"
        header[257:263] = b"ustar\0"
        header[263:265] = b"00"
        checksum = sum(header)
        header[148:156] = f"{checksum:06o}\0 ".encode("ascii")
        archive.extend(header)
        archive.extend(content)
        archive.extend(b"\0" * ((-len(content)) % 512))
    archive.extend(b"\0" * 1024)
    return bytes(archive)


def build_publish_request() -> bytes:
    manifest = b"""[pafio]
manifest-version = 1

[package]
name = "smoke/check"
version = "0.0.1"
edition = "2026"
publish = true

[build]
implicit-std = true

[lib]
path = "src/lib.styio"
"""
    archive_bytes = _deterministic_pafio_ustar(
        (
            ("check-0.0.1/pafio.toml", manifest),
            ("check-0.0.1/src/lib.styio", b"# smoke := 1\n"),
        )
    )
    return json.dumps(
        {
            "package": "smoke/check",
            "version": "0.0.1",
            "archive_name": "smoke-check-0.0.1.pafio.src.tar",
            "archive_base64": base64.b64encode(archive_bytes).decode("ascii"),
            "archive_sha256": hashlib.sha256(archive_bytes).hexdigest(),
            "archive_size_bytes": len(archive_bytes),
            "publisher_id": "vm-smoke-client-claim",
            "dependencies": [],
            "dev_dependencies": [],
        },
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def load_json_url(url: str, *, timeout: float, method: str = "GET", body: bytes | None = None) -> dict[str, Any]:
    headers = {"Accept": "application/json"}
    if body is not None:
        headers["Content-Type"] = "application/json"
    request = Request(url, data=body, headers=headers, method=method)
    with urlopen(request, timeout=timeout) as response:
        payload = response.read()
    return json.loads(payload.decode("utf-8"))


def load_bytes_url(url: str, *, timeout: float) -> bytes:
    with urlopen(url, timeout=timeout) as response:
        return response.read()


def run_smoke(control_url: str, read_url: str, *, timeout: float) -> dict[str, Any]:
    control = control_url.rstrip("/")
    read = read_url.rstrip("/")
    status = load_json_url(f"{control}{BASE_PATH}/status", timeout=timeout)
    descriptor = load_json_url(f"{control}{BASE_PATH}/descriptor", timeout=timeout)
    try:
        publish = load_json_url(
            f"{control}{BASE_PATH}/publish",
            timeout=timeout,
            method="POST",
            body=build_publish_request(),
        )
        publish_status = 200
    except HTTPError as error:
        try:
            publish_status = error.code
            publish = json.loads(error.read().decode("utf-8"))
        finally:
            error.close()
    verify = load_json_url(f"{control}{BASE_PATH}/verify", timeout=timeout, method="POST", body=b"{}")
    config = json.loads(load_bytes_url(f"{read}/config.json", timeout=timeout).decode("utf-8"))
    root = json.loads(load_bytes_url(f"{read}/trust/root.json", timeout=timeout).decode("utf-8"))
    ok = (
        status.get("returncode") == 0
        and publish_status in (200, 409)
        and verify.get("returncode") == 0
        and descriptor.get("returncode") == 0
        and config.get("protocol") == "pafio-static-registry"
        and config.get("protocol_version") == 2
        and isinstance(root.get("signed"), dict)
    )
    return {
        "ok": ok,
        "control_status": status,
        "control_descriptor": descriptor,
        "control_publish": publish,
        "control_publish_status": publish_status,
        "control_verify": verify,
        "read_config": {
            "registry_name": config.get("registry_name"),
            "protocol": config.get("protocol"),
            "protocol_version": config.get("protocol_version"),
        },
        "read_root_version": root.get("signed", {}).get("version"),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run a VM smoke check against a deployed Pafio registry server node.")
    parser.add_argument("--control-url", required=True, help="Base URL for the registry control plane.")
    parser.add_argument("--read-url", required=True, help="Base URL for the registry static read plane.")
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--json", action="store_true", help="Print the full JSON smoke report.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        report = run_smoke(args.control_url, args.read_url, timeout=args.timeout)
    except (HTTPError, URLError, TimeoutError, json.JSONDecodeError, OSError) as err:
        report = {"ok": False, "error": str(err)}
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    elif report.get("ok"):
        print("registry VM smoke passed")
    else:
        print("registry VM smoke failed", file=sys.stderr)
    return 0 if report.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
