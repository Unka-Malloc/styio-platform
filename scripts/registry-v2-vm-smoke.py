#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


BASE_PATH = "/api/spio-registry-control/v1"


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
    verify = load_json_url(f"{control}{BASE_PATH}/verify", timeout=timeout, method="POST", body=b"{}")
    config = json.loads(load_bytes_url(f"{read}/config.json", timeout=timeout).decode("utf-8"))
    root = json.loads(load_bytes_url(f"{read}/trust/root.json", timeout=timeout).decode("utf-8"))
    ok = (
        status.get("returncode") == 0
        and verify.get("returncode") == 0
        and config.get("protocol") == "spio-static-registry"
        and config.get("protocol_version") == 2
        and isinstance(root.get("signed"), dict)
    )
    return {
        "ok": ok,
        "control_status": status,
        "control_verify": verify,
        "read_config": {
            "registry_name": config.get("registry_name"),
            "protocol": config.get("protocol"),
            "protocol_version": config.get("protocol_version"),
        },
        "read_root_version": root.get("signed", {}).get("version"),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run a VM smoke check against a deployed spio registry server node.")
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
