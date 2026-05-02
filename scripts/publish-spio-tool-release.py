#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import platform as host_platform
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Any


def die(message: str) -> None:
    raise SystemExit(f"publish-spio-tool-release: {message}")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def atomic_write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent, delete=False) as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")
        temp_path = Path(handle.name)
    temp_path.replace(path)


def atomic_write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent, delete=False) as handle:
        handle.write(content)
        temp_path = Path(handle.name)
    temp_path.replace(path)


def detect_platform() -> str:
    system = host_platform.system().lower()
    machine = host_platform.machine().lower()
    if system == "linux":
        os_name = "linux"
    elif system == "darwin":
        os_name = "darwin"
    else:
        die(f"unsupported OS for automatic platform detection: {host_platform.system()}")

    if machine in {"aarch64", "arm64"}:
        arch = "aarch64"
    elif machine in {"x86_64", "amd64"}:
        arch = "x86_64"
    else:
        die(f"unsupported CPU for automatic platform detection: {host_platform.machine()}")
    return f"{os_name}-{arch}"


def copy_immutable(source: Path, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        if sha256_file(source) != sha256_file(dest):
            die(f"refusing to overwrite existing artifact with different content: {dest}")
        return
    shutil.copy2(source, dest)
    dest.chmod(0o755)


def load_latest(path: Path, *, version: str, tool: str) -> dict[str, Any]:
    if not path.exists():
        return {
            "schema_version": 1,
            "tool": tool,
            "version": version,
            "platforms": {},
        }
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as err:
        die(f"latest metadata is not valid JSON: {path}: {err}")
    if not isinstance(payload, dict):
        die(f"latest metadata must be a JSON object: {path}")
    if payload.get("version") != version:
        payload = {
            "schema_version": 1,
            "tool": tool,
            "version": version,
            "platforms": {},
        }
    platforms = payload.setdefault("platforms", {})
    if not isinstance(platforms, dict):
        die(f"latest metadata platforms must be an object: {path}")
    payload["schema_version"] = 1
    payload["tool"] = tool
    payload["version"] = version
    return payload


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Publish a prebuilt tool binary into a registry read-plane root.")
    parser.add_argument("--registry-root", required=True, help="Static read-plane root that serves config.json and tools/.")
    parser.add_argument("--binary", required=True, help="Tool executable to publish.")
    parser.add_argument("--version", required=True, help="Release version, for example 0.1.0-dev.")
    parser.add_argument("--platform", default="", help="Release platform key. Defaults to host detection.")
    parser.add_argument(
        "--install-script",
        default="",
        help="Optional install-spio.sh to publish at tools/spio/install-spio.sh.",
    )
    parser.add_argument(
        "--channel",
        default="latest",
        help="Channel pointer to update under tools/<tool>/channel/<channel>/<platform>/version.",
    )
    parser.add_argument("--tool", default="spio", help="Tool namespace under tools/.")
    parser.add_argument("--binary-name", default="spio", help="Published executable filename.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    registry_root = Path(args.registry_root).expanduser().resolve()
    binary = Path(args.binary).expanduser().resolve()
    if not binary.is_file():
        die(f"binary does not exist: {binary}")
    if not registry_root.exists() or not registry_root.is_dir():
        die(f"registry root is not a directory: {registry_root}")

    platform_key = args.platform or detect_platform()
    tool_root = registry_root / "tools" / args.tool
    relative_binary_path = Path("tools") / args.tool / "releases" / args.version / platform_key / args.binary_name
    binary_dest = registry_root / relative_binary_path
    copy_immutable(binary, binary_dest)
    digest = sha256_file(binary_dest)
    atomic_write_text(binary_dest.parent / f"{args.binary_name}.sha256", f"{digest}\n")

    if args.install_script:
        install_script = Path(args.install_script).expanduser().resolve()
        if not install_script.is_file():
            die(f"install script does not exist: {install_script}")
        install_dest = tool_root / "install-spio.sh"
        install_dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(install_script, install_dest)
        install_dest.chmod(0o755)

    channel_version_path = tool_root / "channel" / args.channel / platform_key / "version"
    atomic_write_text(channel_version_path, f"{args.version}\n")

    latest_path = tool_root / "latest.json"
    latest = load_latest(latest_path, version=args.version, tool=args.tool)
    latest["platforms"][platform_key] = {
        "path": relative_binary_path.as_posix(),
        "sha256": digest,
        "size_bytes": binary_dest.stat().st_size,
    }
    atomic_write_json(latest_path, latest)

    result = {
        "ok": True,
        "tool": args.tool,
        "version": args.version,
        "platform": platform_key,
        "binary_path": relative_binary_path.as_posix(),
        "sha256": digest,
        "channel": args.channel,
        "channel_version_path": channel_version_path.relative_to(registry_root).as_posix(),
        "latest_path": latest_path.relative_to(registry_root).as_posix(),
    }
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
