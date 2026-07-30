#!/usr/bin/env python3

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
CONTRACT_PATH = ROOT / "contracts/registry-control-plane/v1/registry-control-plane.contract.json"
EXAMPLES_PATH = ROOT / "contracts/registry-control-plane/v1/registry-control-plane.examples.json"

EXPECTED_OPERATION_SNAPSHOT = [
    ("registryStatus", "GET", "/status"),
    ("registryDescriptor", "GET", "/descriptor"),
    ("publishRelease", "POST", "/publish"),
    ("verifyRegistry", "POST", "/verify"),
    ("getPackage", "GET", "/packages/{namespace}/{name}"),
    ("listPackageReleases", "GET", "/packages/{namespace}/{name}/releases"),
    ("getPackageRelease", "GET", "/packages/{namespace}/{name}/releases/{version}"),
    ("yankPackageRelease", "POST", "/packages/{namespace}/{name}/releases/{version}/yank"),
    ("unyankPackageRelease", "POST", "/packages/{namespace}/{name}/releases/{version}/unyank"),
    ("listPackageOwners", "GET", "/packages/{namespace}/{name}/owners"),
    ("addPackageOwner", "POST", "/packages/{namespace}/{name}/owners"),
    ("removePackageOwner", "DELETE", "/packages/{namespace}/{name}/owners/{owner_id}"),
    ("createPublishToken", "POST", "/tokens"),
    ("listPublishTokens", "GET", "/tokens"),
    ("revokePublishToken", "DELETE", "/tokens/{token_id}"),
    ("listRepositories", "GET", "/repositories"),
    ("listRepositoryVersions", "GET", "/repositories/{repository_id}/versions"),
    ("getPublication", "GET", "/publications/{publication_id}"),
    ("verifyPublication", "POST", "/publications/{publication_id}/verify"),
    ("listDistributions", "GET", "/distributions"),
    ("promoteDistribution", "POST", "/distributions/{distribution_id}/promote"),
    ("rollbackDistribution", "POST", "/distributions/{distribution_id}/rollback"),
    ("listReleaseChannels", "GET", "/release-channels"),
    ("rolloutReleaseChannel", "POST", "/release-channels/{channel}/rollout"),
]


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def resolve_shape(contract: dict[str, Any], name: str) -> dict[str, Any]:
    shapes = contract.get("shapes", {})
    if name not in shapes:
        raise KeyError(f"unknown shape: {name}")
    return shapes[name]


def is_integer(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def is_iso_datetime(value: str) -> bool:
    from datetime import datetime
    candidate = value[:-1] + "+00:00" if value.endswith("Z") else value
    try:
        datetime.fromisoformat(candidate)
        return True
    except ValueError:
        return False


def validate_spec(contract: dict[str, Any], spec: dict[str, Any], value: Any, path: str, errors: list[str]) -> None:
    if "ref" in spec:
        validate_shape(contract, spec["ref"], value, path, errors)
        return

    spec_type = spec.get("type")
    if spec_type == "any":
        return
    if spec_type == "string":
        if not isinstance(value, str):
            errors.append(f"{path}: expected string")
            return
        min_length = spec.get("min_length")
        if min_length is not None and len(value) < int(min_length):
            errors.append(f"{path}: expected string length >= {min_length}")
        max_length = spec.get("max_length")
        if max_length is not None and len(value) > int(max_length):
            errors.append(f"{path}: expected string length <= {max_length}")
        suffix = spec.get("suffix")
        if suffix is not None and not value.endswith(str(suffix)):
            errors.append(f"{path}: expected suffix {suffix!r}")
        if spec.get("format") == "date-time" and not is_iso_datetime(value):
            errors.append(f"{path}: expected ISO 8601 date-time string")
        return
    if spec_type == "integer":
        if not is_integer(value):
            errors.append(f"{path}: expected integer")
            return
        minimum = spec.get("minimum")
        if minimum is not None and value < int(minimum):
            errors.append(f"{path}: expected integer >= {minimum}")
        maximum = spec.get("maximum")
        if maximum is not None and value > int(maximum):
            errors.append(f"{path}: expected integer <= {maximum}")
        return
    if spec_type == "boolean":
        if not isinstance(value, bool):
            errors.append(f"{path}: expected boolean")
        return
    if spec_type == "array":
        if not isinstance(value, list):
            errors.append(f"{path}: expected array")
            return
        max_items = spec.get("max_items")
        if max_items is not None and len(value) > int(max_items):
            errors.append(f"{path}: expected at most {max_items} items")
        item_spec = spec.get("items")
        if not isinstance(item_spec, dict):
            errors.append(f"{path}: array items spec is missing")
            return
        for index, item in enumerate(value):
            validate_spec(contract, item_spec, item, f"{path}[{index}]", errors)
        return
    errors.append(f"{path}: unsupported spec type {spec_type!r}")


def validate_shape(contract: dict[str, Any], shape_name: str, value: Any, path: str, errors: list[str]) -> None:
    shape = resolve_shape(contract, shape_name)
    kind = shape.get("kind")
    if kind == "none":
        if value is not None:
            errors.append(f"{path}: expected no request body")
        return
    if kind != "object":
        errors.append(f"{path}: unsupported shape kind {kind!r}")
        return
    if not isinstance(value, dict):
        errors.append(f"{path}: expected object")
        return
    required = shape.get("required", {})
    optional = shape.get("optional", {})
    allowed = set(required) | set(optional)
    allow_additional = bool(shape.get("allow_additional", False))
    for field_name, field_spec in required.items():
        if field_name not in value:
            errors.append(f"{path}: missing required field {field_name!r}")
            continue
        validate_spec(contract, field_spec, value[field_name], f"{path}.{field_name}", errors)
    for field_name, field_value in value.items():
        if field_name not in allowed:
            if allow_additional:
                continue
            errors.append(f"{path}: unexpected field {field_name!r}")
            continue
        if field_name in optional:
            validate_spec(contract, optional[field_name], field_value, f"{path}.{field_name}", errors)


def assert_valid(contract: dict[str, Any], shape_name: str, value: Any, label: str) -> None:
    errors: list[str] = []
    validate_shape(contract, shape_name, value, label, errors)
    if errors:
        raise AssertionError("\n".join(errors))


def main() -> int:
    contract = load_json(CONTRACT_PATH)
    examples = load_json(EXAMPLES_PATH)
    if contract.get("schema_version") != 1:
        print("schema_version must be 1", file=sys.stderr)
        return 1
    if contract.get("base_path") != "/api/pafio-registry-control/v1":
        print("base_path drift detected", file=sys.stderr)
        return 1
    snapshot = [(item["id"], item["method"], item["path"]) for item in contract["operations"]]
    if snapshot != EXPECTED_OPERATION_SNAPSHOT:
        print(f"operation snapshot drift detected: {snapshot!r}", file=sys.stderr)
        return 1
    publish_request = resolve_shape(contract, "PublishRequest")
    expected_publish_fields = {
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
    if set(publish_request.get("required", {})) != expected_publish_fields or publish_request.get("optional") != {}:
        print("PublishRequest must require exactly the nine Pafio archive upload fields", file=sys.stderr)
        return 1
    publish_payload = resolve_shape(contract, "PublishPayload")
    exposed_paths = {"registry_root", "registry_read_root", "key_dir", "archive_path", "staging_path"}
    payload_fields = set(publish_payload.get("required", {})) | set(publish_payload.get("optional", {}))
    if payload_fields & exposed_paths:
        print(f"PublishPayload exposes local path fields: {sorted(payload_fields & exposed_paths)!r}", file=sys.stderr)
        return 1
    for dependency_field in ("dependencies", "dev_dependencies"):
        if publish_payload["required"].get(dependency_field, {}).get("type") != "array":
            print(f"PublishPayload.{dependency_field} must remain an array", file=sys.stderr)
            return 1
    publish_example = examples.get("publishRelease", {})
    request_example = publish_example.get("request", {})
    success_payload = publish_example.get("success", {}).get("payload", {})
    if request_example.get("publisher_id") == success_payload.get("publisher_id"):
        print("publish example must prove the client publisher_id is not authoritative", file=sys.stderr)
        return 1
    if not str(success_payload.get("artifact_path", "")).endswith(".pafio.src.tar"):
        print("publish example artifact must use .pafio.src.tar", file=sys.stderr)
        return 1
    if any(field in success_payload for field in exposed_paths):
        print("publish example leaks a local path field", file=sys.stderr)
        return 1
    for operation in contract["operations"]:
        operation_id = operation["id"]
        if operation_id not in examples:
            print(f"missing examples for {operation_id}", file=sys.stderr)
            return 1
        pack = examples[operation_id]
        assert_valid(contract, operation["request_shape"], pack.get("request"), f"{operation_id}.request")
        assert_valid(contract, operation["success_shape"], pack["success"], f"{operation_id}.success")
        assert_valid(contract, operation["failure_shape"], pack["failure"], f"{operation_id}.failure")
    print(f"[registry-control-plane-contract-gate] ok base_path={contract['base_path']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
