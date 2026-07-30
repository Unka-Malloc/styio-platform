#!/usr/bin/env python3

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import json
import os
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
PACKAGE_REGISTRY = ROOT / "src" / "PlatformCloud" / "PackageRegistry"
import sys

if str(PACKAGE_REGISTRY) not in sys.path:
    sys.path.insert(0, str(PACKAGE_REGISTRY))

from PublicationBuilder.package_registry_v2 import publish_to_registry_v2, validate_publish_archive_bytes  # noqa: E402
from StaticReadPlane.package_registry_v2 import verify_registry_root  # noqa: E402
from package_registry_v2.common import RegistryV2Error  # noqa: E402


BASE_PATH = "/api/pafio-registry-control/v1"
DEFAULT_MAX_REQUEST_BYTES = 1 * 1024 * 1024
PUBLISH_MAX_REQUEST_BYTES = 96 * 1024 * 1024
MAX_ARCHIVE_BYTES = 64 * 1024 * 1024
MAX_ARCHIVE_BASE64_BYTES = 4 * ((MAX_ARCHIVE_BYTES + 2) // 3)
MAX_DEPENDENCIES_PER_TABLE = 256
MAX_PACKAGE_BYTES = 255
MAX_VERSION_BYTES = 64
MAX_ALIAS_BYTES = 128
MAX_REGISTRY_BYTES = 2048
MAX_ARCHIVE_NAME_BYTES = 255
MAX_PUBLISHER_BYTES = 255
SERVER_PUBLISHER_ID = "control-plane"
REQUEST_TIMEOUT_SECONDS = 10.0
PUBLISH_FIELDS = {
    "package",
    "version",
    "archive_name",
    "archive_base64",
    "archive_sha256",
    "archive_size_bytes",
    "publisher_id",
    "dependencies",
    "dev_dependencies",
}
DEPENDENCY_FIELDS = {"alias", "package", "version_req", "registry"}


def registry_error_status(error: RegistryV2Error, *, operation: str) -> int:
    detail = str(error)
    if "already published" in detail or "already exists" in detail:
        return 409
    if operation == "publish":
        return 422
    if operation == "verify":
        return 422
    return 500


def load_json_request(
    handler: BaseHTTPRequestHandler,
    *,
    max_request_bytes: int = DEFAULT_MAX_REQUEST_BYTES,
) -> dict[str, Any]:
    content_length = handler.headers.get("Content-Length")
    if content_length is None:
        return {}
    try:
        length = int(content_length)
    except ValueError as err:
        raise ValueError("request body content-length must be an integer") from err
    if length < 0:
        raise ValueError("request body content-length must be non-negative")
    if length > max_request_bytes:
        raise ValueError(f"request body exceeds {max_request_bytes} bytes")
    try:
        body = handler.rfile.read(length)
    except TimeoutError as err:
        raise ValueError(f"request body read timed out after {REQUEST_TIMEOUT_SECONDS} seconds") from err
    if not body:
        return {}
    if len(body) != length:
        raise ValueError("request body was truncated")
    try:
        payload = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as err:
        raise ValueError("request body must be valid UTF-8 JSON") from err
    if not isinstance(payload, dict):
        raise ValueError("request body must be a JSON object")
    return payload


def bounded_string(value: Any, field: str, max_bytes: int) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be a non-empty string")
    if len(value.encode("utf-8")) > max_bytes:
        raise ValueError(f"{field} exceeds the {max_bytes}-byte limit")
    return value


def safe_package_name(value: str) -> bool:
    parts = value.split("/")
    if len(parts) != 2:
        return False
    for part in parts:
        if not part or not (("a" <= part[0] <= "z") or ("0" <= part[0] <= "9")):
            return False
        if any(not (("a" <= char <= "z") or ("0" <= char <= "9") or char in "-_") for char in part):
            return False
    return True


def safe_archive_name(value: str) -> bool:
    return (
        not value.startswith(".")
        and value.endswith(".pafio.src.tar")
        and all(
            ("A" <= char <= "Z")
            or ("a" <= char <= "z")
            or ("0" <= char <= "9")
            or char in "-_."
            for char in value
        )
    )


def strict_pafio_version(value: str) -> bool:
    parts = value.split(".")
    return (
        len(parts) == 3
        and all(part and all("0" <= char <= "9" for char in part) for part in parts)
    )


def normalize_dependencies(value: Any, field: str) -> list[dict[str, str]]:
    if not isinstance(value, list):
        raise ValueError(f"{field} must be an array")
    if len(value) > MAX_DEPENDENCIES_PER_TABLE:
        raise ValueError(f"{field} exceeds the {MAX_DEPENDENCIES_PER_TABLE}-entry limit")
    normalized: list[dict[str, str]] = []
    aliases: set[str] = set()
    for entry in value:
        if not isinstance(entry, dict) or set(entry) != DEPENDENCY_FIELDS:
            raise ValueError(f"{field} entries must contain exactly alias, package, version_req, and registry")
        alias = bounded_string(entry["alias"], f"{field}.alias", MAX_ALIAS_BYTES)
        package = bounded_string(entry["package"], f"{field}.package", MAX_PACKAGE_BYTES)
        version_req = bounded_string(entry["version_req"], f"{field}.version_req", MAX_VERSION_BYTES)
        registry = bounded_string(entry["registry"], f"{field}.registry", MAX_REGISTRY_BYTES)
        if not safe_package_name(package):
            raise ValueError(f"{field}.package must use lowercase namespace/name form")
        if not strict_pafio_version(version_req):
            raise ValueError(f"{field}.version_req must be strict x.y.z")
        if alias in aliases:
            raise ValueError(f"{field} contains duplicate alias: {alias}")
        aliases.add(alias)
        normalized.append(
            {
                "alias": alias,
                "package": package,
                "version_req": version_req,
                "registry": registry,
            }
        )
    return sorted(normalized, key=lambda dependency: dependency["alias"])


def validate_publish_request(payload: dict[str, Any]) -> dict[str, Any]:
    if set(payload) != PUBLISH_FIELDS:
        missing = sorted(PUBLISH_FIELDS - set(payload))
        unknown = sorted(set(payload) - PUBLISH_FIELDS)
        if missing:
            raise ValueError(f"publish request is missing required field: {missing[0]}")
        raise ValueError(f"publish request contains unknown field: {unknown[0]}")

    package = bounded_string(payload["package"], "package", MAX_PACKAGE_BYTES)
    version = bounded_string(payload["version"], "version", MAX_VERSION_BYTES)
    archive_name = bounded_string(payload["archive_name"], "archive_name", MAX_ARCHIVE_NAME_BYTES)
    archive_base64 = bounded_string(payload["archive_base64"], "archive_base64", MAX_ARCHIVE_BASE64_BYTES)
    archive_sha256 = bounded_string(payload["archive_sha256"], "archive_sha256", 64)
    bounded_string(payload["publisher_id"], "publisher_id", MAX_PUBLISHER_BYTES)
    if not safe_package_name(package):
        raise ValueError("package must use lowercase namespace/name form")
    if not strict_pafio_version(version):
        raise ValueError("version must be strict x.y.z")
    if not safe_archive_name(archive_name):
        raise ValueError("archive_name must be a safe basename ending in .pafio.src.tar")
    if len(archive_sha256) != 64 or any(char not in "0123456789abcdef" for char in archive_sha256):
        raise ValueError("archive_sha256 must be a lowercase SHA-256 digest")
    archive_size = payload["archive_size_bytes"]
    if not isinstance(archive_size, int) or isinstance(archive_size, bool):
        raise ValueError("archive_size_bytes must be an integer")
    if archive_size <= 0 or archive_size > MAX_ARCHIVE_BYTES:
        raise ValueError(f"archive_size_bytes must be between 1 and {MAX_ARCHIVE_BYTES}")
    try:
        encoded = archive_base64.encode("ascii")
        archive_bytes = base64.b64decode(encoded, validate=True)
    except (UnicodeEncodeError, binascii.Error, ValueError) as err:
        raise ValueError("archive_base64 must be canonical base64") from err
    if base64.b64encode(archive_bytes) != encoded:
        raise ValueError("archive_base64 must be canonical base64")
    if len(archive_bytes) != archive_size:
        raise ValueError("archive_base64 decoded size does not match archive_size_bytes")
    if not archive_bytes or len(archive_bytes) > MAX_ARCHIVE_BYTES:
        raise ValueError(f"decoded archive must be between 1 and {MAX_ARCHIVE_BYTES} bytes")
    if hashlib.sha256(archive_bytes).hexdigest() != archive_sha256:
        raise ValueError("decoded archive SHA-256 does not match archive_sha256")
    return {
        "package": package,
        "version": version,
        "archive_name": archive_name,
        "archive_bytes": archive_bytes,
        "dependencies": normalize_dependencies(payload["dependencies"], "dependencies"),
        "dev_dependencies": normalize_dependencies(payload["dev_dependencies"], "dev_dependencies"),
    }


def write_private_staging_archive(registry_root: str, archive_bytes: bytes) -> Path:
    staging_root = Path(registry_root) / "_staging" / "uploads"
    staging_root.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(staging_root, 0o700)
    descriptor, path_value = tempfile.mkstemp(
        prefix="publish-",
        suffix=".pafio.src.tar",
        dir=staging_root,
    )
    path = Path(path_value)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "wb") as handle:
            descriptor = -1
            handle.write(archive_bytes)
            handle.flush()
            os.fsync(handle.fileno())
    except Exception:
        if descriptor >= 0:
            os.close(descriptor)
        path.unlink(missing_ok=True)
        raise
    return path


def success_envelope(message: str, payload: dict[str, Any]) -> dict[str, Any]:
    return {
        "returncode": 0,
        "message": message,
        "stdout": "",
        "stderr": "",
        "payload": payload,
    }


def failure_envelope(message: str, detail: str, *, category: str, returncode: int = 17) -> dict[str, Any]:
    return {
        "returncode": returncode,
        "message": message,
        "stdout": "",
        "stderr": detail,
        "error_payload": {
            "category": category,
            "detail": detail,
        },
    }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


class RegistryControlPlaneHandler(BaseHTTPRequestHandler):
    registry_root: str
    key_dir: str
    registry_name: str
    read_root_url: str = ""
    control_plane_base_url: str = ""

    def setup(self) -> None:
        super().setup()
        self.connection.settimeout(REQUEST_TIMEOUT_SECONDS)

    def _send_json(self, status_code: int, payload: dict[str, Any]) -> None:
        body = json.dumps(payload, indent=2, sort_keys=True, ensure_ascii=False).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: Any) -> None:
        return

    def do_GET(self) -> None:
        if self.path == f"{BASE_PATH}/status":
            self._handle_status()
            return
        if self.path == f"{BASE_PATH}/descriptor":
            self._handle_descriptor()
            return
        self.send_error(404, "not found")

    def _handle_status(self) -> None:
        root_path = Path(self.registry_root)
        payload = {
            "registry_root": "<redacted>",
            "key_dir": "<redacted>",
            "registry_name": self.registry_name,
            "root_initialized": (root_path / "config.json").exists() and (root_path / "trust" / "root.json").exists(),
            "config_present": (root_path / "config.json").exists(),
            "root_metadata_present": (root_path / "trust" / "root.json").exists(),
            "publish_endpoint": f"{BASE_PATH}/publish",
            "verify_endpoint": f"{BASE_PATH}/verify",
            "descriptor_endpoint": f"{BASE_PATH}/descriptor",
        }
        self._send_json(200, success_envelope("registry control plane is ready", payload))

    def _handle_descriptor(self) -> None:
        root_path = Path(self.registry_root)
        root_metadata = root_path / "trust" / "root.json"
        if not root_metadata.exists():
            self._send_json(
                422,
                failure_envelope(
                    "registry descriptor failed",
                    "registry root metadata is not initialized",
                    category="RegistryDescriptorError",
                ),
            )
            return
        registry_root = self.read_root_url or root_path.resolve().as_uri()
        control_plane_base = self.control_plane_base_url or BASE_PATH
        payload = {
            "schema_version": 1,
            "registry_name": self.registry_name,
            "registry_root": registry_root,
            "control_plane_base_url": control_plane_base,
            "root_sha256": sha256_file(root_metadata),
            "issued_at": "2026-05-02T00:00:00Z",
            "expires": "2026-06-02T00:00:00Z",
            "descriptor_signature": "platform-control-plane-mtls",
        }
        self._send_json(200, success_envelope("published registry trust descriptor", payload))

    def do_POST(self) -> None:
        if self.path == f"{BASE_PATH}/publish":
            self._handle_publish()
            return
        if self.path == f"{BASE_PATH}/verify":
            self._handle_verify()
            return
        self.send_error(404, "not found")

    def _handle_publish(self) -> None:
        try:
            request = load_json_request(self, max_request_bytes=PUBLISH_MAX_REQUEST_BYTES)
            validated = validate_publish_request(request)
        except ValueError as err:
            self._send_json(400, failure_envelope("malformed registry publish request", str(err), category="UsageError", returncode=2))
            return
        staged_archive: Path | None = None
        try:
            validate_publish_archive_bytes(
                validated["archive_bytes"],
                package_name=validated["package"],
                package_version=validated["version"],
                dependencies=validated["dependencies"],
                dev_dependencies=validated["dev_dependencies"],
            )
            staged_archive = write_private_staging_archive(self.registry_root, validated["archive_bytes"])
            payload = publish_to_registry_v2(
                self.registry_root,
                self.key_dir,
                archive_path_value=str(staged_archive),
                archive_name=validated["archive_name"],
                package_name=validated["package"],
                package_version=validated["version"],
                dependencies=validated["dependencies"],
                dev_dependencies=validated["dev_dependencies"],
                registry_name=self.registry_name,
                # This VM helper is deployed behind the authenticated Platform
                # proxy. The client claim is required by contract but is never
                # authoritative for ownership or persisted publisher identity.
                publisher_id=SERVER_PUBLISHER_ID,
            )
        except RegistryV2Error as err:
            detail = str(err)
            for sensitive in (self.registry_root, self.key_dir, str(staged_archive or "")):
                if sensitive and sensitive in detail:
                    detail = "registry publication validation failed"
                    break
            self._send_json(
                registry_error_status(err, operation="publish"),
                failure_envelope("registry publish failed", detail, category="PublishError"),
            )
            return
        except OSError:
            self._send_json(
                500,
                failure_envelope(
                    "registry publish failed",
                    "registry publication staging failed",
                    category="PublishError",
                ),
            )
            return
        finally:
            if staged_archive is not None:
                staged_archive.unlink(missing_ok=True)
        self._send_json(200, success_envelope("published registry v2 release", payload))

    def _handle_verify(self) -> None:
        try:
            request = load_json_request(self, max_request_bytes=DEFAULT_MAX_REQUEST_BYTES)
        except ValueError as err:
            self._send_json(400, failure_envelope("malformed registry verify request", str(err), category="UsageError", returncode=2))
            return
        if request not in ({},):
            self._send_json(
                400,
                failure_envelope(
                    "registry verification failed",
                    "verify request must be an empty JSON object",
                    category="VerifyError",
                    returncode=2,
                ),
            )
            return
        try:
            payload = verify_registry_root(self.registry_root)
        except RegistryV2Error as err:
            self._send_json(
                registry_error_status(err, operation="verify"),
                failure_envelope("registry verification failed", str(err), category="VerifyError"),
            )
            return
        self._send_json(200, success_envelope("verified registry v2 root", payload))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the local HTTP control plane for a Pafio static registry root.")
    parser.add_argument("--root", required=True, help="Local directory bound as the registry v2 static root.")
    parser.add_argument("--key-dir", required=True, help="Directory containing the registry v2 role keys.")
    parser.add_argument("--registry-name", default="pafio-static-registry", help="Registry name used when the root is initialized.")
    parser.add_argument("--read-root-url", default="", help="Public static read root written into registry trust descriptors.")
    parser.add_argument("--control-plane-base-url", default="", help="Public control-plane base URL written into registry trust descriptors.")
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    RegistryControlPlaneHandler.registry_root = str(Path(args.root).resolve())
    RegistryControlPlaneHandler.key_dir = str(Path(args.key_dir).resolve())
    RegistryControlPlaneHandler.registry_name = args.registry_name
    RegistryControlPlaneHandler.read_root_url = args.read_root_url
    RegistryControlPlaneHandler.control_plane_base_url = args.control_plane_base_url
    server = ThreadingHTTPServer((args.bind, args.port), RegistryControlPlaneHandler)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
