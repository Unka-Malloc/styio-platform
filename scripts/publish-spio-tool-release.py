#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform as host_platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

STYIO_RELEASE_TARGETS = {
    "styio-linux",
    "styio-windows-cli",
    "styio-windows-desktop-gui",
    "styio-macos-cli",
    "styio-macos-desktop-gui",
    "styio-ios",
    "styio-android",
}


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
        os_name = "linux-musl" if detect_linux_libc() == "musl" else "linux"
    elif system == "darwin":
        os_name = "darwin"
    elif system == "windows":
        os_name = "windows"
    else:
        die(f"unsupported OS for automatic platform detection: {host_platform.system()}")

    if machine in {"aarch64", "arm64"}:
        arch = "aarch64"
    elif machine in {"x86_64", "amd64"}:
        arch = "x86_64"
    else:
        die(f"unsupported CPU for automatic platform detection: {host_platform.machine()}")
    return f"{os_name}-{arch}"


def detect_linux_libc() -> str:
    override = os.environ.get("STYIO_PLATFORM_RELEASE_LIBC", "").strip().lower()
    if override:
        if override in {"glibc", "musl"}:
            return override
        die(f"unsupported STYIO_PLATFORM_RELEASE_LIBC value: {override}")

    if Path("/etc/alpine-release").exists():
        return "musl"

    libc_name, _ = host_platform.libc_ver()
    normalized = libc_name.lower()
    if "musl" in normalized:
        return "musl"
    if "glibc" in normalized:
        return "glibc"

    try:
        proc = subprocess.run(["ldd", "--version"], capture_output=True, text=True, check=False)
    except OSError:
        return "glibc"
    ldd_text = (proc.stdout + proc.stderr).lower()
    if "musl" in ldd_text:
        return "musl"
    return "glibc"


def infer_styio_release_target(platform_key: str) -> str:
    if platform_key.startswith(("linux-", "linux-musl-")):
        return "styio-linux"
    if platform_key.startswith("darwin-"):
        return "styio-macos-cli"
    if platform_key.startswith("windows-"):
        return "styio-windows-cli"
    if platform_key.startswith("ios-"):
        return "styio-ios"
    if platform_key.startswith("android-"):
        return "styio-android"
    die(f"cannot infer styio release target from platform: {platform_key}")


def validate_styio_release_target(target: str, platform_key: str) -> None:
    if target not in STYIO_RELEASE_TARGETS:
        die(
            "unsupported styio release target: "
            + target
            + " (expected one of "
            + ", ".join(sorted(STYIO_RELEASE_TARGETS))
            + ")"
        )
    if target == "styio-linux" and not platform_key.startswith(("linux-", "linux-musl-")):
        die(f"release target {target} requires a linux or linux-musl platform, got {platform_key}")
    if target.startswith("styio-windows-") and not platform_key.startswith("windows-"):
        die(f"release target {target} requires a windows platform, got {platform_key}")
    if target.startswith("styio-macos-") and not platform_key.startswith("darwin-"):
        die(f"release target {target} requires a darwin platform, got {platform_key}")
    if target == "styio-ios" and not platform_key.startswith("ios-"):
        die(f"release target {target} requires an ios platform, got {platform_key}")
    if target == "styio-android" and not platform_key.startswith("android-"):
        die(f"release target {target} requires an android platform, got {platform_key}")


def resolve_release_target(tool: str, platform_key: str, explicit_target: str) -> str:
    if explicit_target:
        if tool != "styio":
            die("--release-target is currently supported only for --tool styio")
        validate_styio_release_target(explicit_target, platform_key)
        return explicit_target
    if tool == "styio":
        target = infer_styio_release_target(platform_key)
        validate_styio_release_target(target, platform_key)
        return target
    return tool


def copy_immutable(source: Path, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        if sha256_file(source) != sha256_file(dest):
            die(f"refusing to overwrite existing artifact with different content: {dest}")
        return
    shutil.copy2(source, dest)
    dest.chmod(0o755)


def load_latest(path: Path, *, version: str, tool: str, release_target: str) -> dict[str, Any]:
    if not path.exists():
        return {
            "schema_version": 1,
            "tool": tool,
            "release_target": release_target,
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
            "release_target": release_target,
            "version": version,
            "platforms": {},
        }
    platforms = payload.setdefault("platforms", {})
    if not isinstance(platforms, dict):
        die(f"latest metadata platforms must be an object: {path}")
    payload["schema_version"] = 1
    payload["tool"] = tool
    payload["release_target"] = release_target
    payload["version"] = version
    return payload


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Publish a prebuilt tool binary into a registry read-plane root.")
    parser.add_argument("--registry-root", required=True, help="Static read-plane root that serves config.json and tools/.")
    parser.add_argument("--binary", required=True, help="Tool executable to publish.")
    parser.add_argument("--version", required=True, help="Release version, for example 0.1.0-dev.")
    parser.add_argument("--platform", default="", help="Release platform key. Defaults to host detection.")
    parser.add_argument(
        "--release-target",
        default="",
        help=(
            "Client release target namespace for styio, for example styio-linux, "
            "styio-macos-cli, or styio-windows-desktop-gui. Defaults from --platform for --tool styio."
        ),
    )
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
    release_target = resolve_release_target(args.tool, platform_key, args.release_target)
    tool_root = registry_root / "tools" / release_target
    relative_binary_path = Path("tools") / release_target / "releases" / args.version / platform_key / args.binary_name
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
    latest = load_latest(latest_path, version=args.version, tool=args.tool, release_target=release_target)
    latest["platforms"][platform_key] = {
        "path": relative_binary_path.as_posix(),
        "sha256": digest,
        "size_bytes": binary_dest.stat().st_size,
    }
    atomic_write_json(latest_path, latest)

    result = {
        "ok": True,
        "tool": args.tool,
        "release_target": release_target,
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
