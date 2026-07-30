from __future__ import annotations

import hashlib
import json
import pathlib
import shutil
import tomllib
from collections import defaultdict
from typing import Any

from package_registry_v2.common import (
    RegistryV2Error,
    artifact_bucket_path,
    atomic_write_bytes,
    canonical_json_bytes,
    ensure_parent,
    expires_in,
    index_path_for_package,
    load_json_file,
    load_role_keys,
    normalize_local_root,
    parse_semver_key,
    require_object,
    require_string,
    sha256_bytes,
    sha256_file,
    sign_payload,
    signed_file_meta,
    split_package_name,
    utc_now,
    write_json_file,
)


MAX_ARCHIVE_MANIFEST_BYTES = 1024 * 1024
MAX_ARCHIVE_BYTES = 64 * 1024 * 1024
MAX_DEPENDENCIES_PER_TABLE = 256
MAX_PACKAGE_BYTES = 255
MAX_VERSION_BYTES = 64
MAX_ALIAS_BYTES = 128
MAX_REGISTRY_BYTES = 2048
MAX_ARCHIVE_NAME_BYTES = 255
USTAR_BLOCK_BYTES = 512


def _bytes_are_zero(value: bytes) -> bool:
    return not any(value)


def _parse_canonical_ustar_octal(field: bytes, name: str) -> int:
    if len(field) < 2 or field[-1] != 0 or any(byte < ord("0") or byte > ord("7") for byte in field[:-1]):
        raise RegistryV2Error(f"source archive {name} must be canonical NUL-terminated octal")
    return int(field[:-1], 8)


def _parse_canonical_ustar_checksum(field: bytes) -> int:
    if len(field) != 8 or field[6] != 0 or field[7] != ord(" "):
        raise RegistryV2Error("source archive checksum field is not canonical ustar octal")
    return _parse_canonical_ustar_octal(field[:7], "checksum")


def _parse_canonical_ustar_text(field: bytes, name: str) -> bytes:
    terminator = field.find(b"\0")
    if terminator < 0:
        return field
    if not _bytes_are_zero(field[terminator:]):
        raise RegistryV2Error(f"source archive {name} has nonzero bytes after its terminator")
    return field[:terminator]


def _validate_canonical_ustar_path(path: bytes) -> list[bytes]:
    if not path or path.startswith(b"/") or b"\\" in path:
        raise RegistryV2Error("source archive member path must be canonical POSIX-relative")
    parts = path.split(b"/")
    if any(part in (b"", b".", b"..") for part in parts):
        raise RegistryV2Error("source archive member path must not contain empty, dot, or parent segments")
    if len(parts) < 2:
        raise RegistryV2Error("source archive members must belong to one top-level package prefix")
    return parts


def _validate_deterministic_pafio_ustar(archive_bytes: bytes) -> bytes:
    archive_size = len(archive_bytes)
    if archive_size < 3 * USTAR_BLOCK_BYTES or archive_size % USTAR_BLOCK_BYTES != 0:
        raise RegistryV2Error("source archive must be 512-byte aligned canonical ustar")

    paths: set[bytes] = set()
    top_level_prefix: bytes | None = None
    previous_path: bytes | None = None
    manifest_bytes = b""
    manifest_count = 0
    offset = 0
    trailer_seen = False
    while offset < archive_size:
        header = archive_bytes[offset : offset + USTAR_BLOCK_BYTES]
        if _bytes_are_zero(header):
            if (
                offset + 2 * USTAR_BLOCK_BYTES != archive_size
                or not _bytes_are_zero(archive_bytes[offset + USTAR_BLOCK_BYTES : offset + 2 * USTAR_BLOCK_BYTES])
            ):
                raise RegistryV2Error("source archive must end with exactly two zero blocks and no trailing bytes")
            trailer_seen = True
            offset += 2 * USTAR_BLOCK_BYTES
            break

        computed_checksum = sum(
            ord(" ") if 148 <= index < 156 else byte
            for index, byte in enumerate(header)
        )
        if _parse_canonical_ustar_checksum(header[148:156]) != computed_checksum:
            raise RegistryV2Error("source archive header checksum mismatch")
        if (
            _parse_canonical_ustar_octal(header[100:108], "mode") != 0o644
            or _parse_canonical_ustar_octal(header[108:116], "uid") != 0
            or _parse_canonical_ustar_octal(header[116:124], "gid") != 0
            or _parse_canonical_ustar_octal(header[136:148], "mtime") != 0
        ):
            raise RegistryV2Error("source archive members must use mode 0644 and zero uid, gid, and mtime")
        if header[156:157] != b"0":
            raise RegistryV2Error("source archive may contain regular files only")
        if header[257:263] != b"ustar\0" or header[263:265] != b"00":
            raise RegistryV2Error("source archive must use POSIX ustar magic and version")
        if (
            not _bytes_are_zero(header[157:257])
            or not _bytes_are_zero(header[265:345])
            or not _bytes_are_zero(header[500:512])
        ):
            raise RegistryV2Error("source archive link, owner, device, and extension fields must be empty")

        name = _parse_canonical_ustar_text(header[0:100], "name")
        prefix = _parse_canonical_ustar_text(header[345:500], "prefix")
        path = name if not prefix else prefix + b"/" + name
        path_parts = _validate_canonical_ustar_path(path)
        if path in paths:
            raise RegistryV2Error("source archive contains a duplicate member path")
        paths.add(path)
        if previous_path is not None and previous_path >= path:
            raise RegistryV2Error("source archive member paths must be strictly increasing")
        previous_path = path
        if top_level_prefix is None:
            top_level_prefix = path_parts[0]
        elif top_level_prefix != path_parts[0]:
            raise RegistryV2Error("source archive members must share one top-level package prefix")

        file_size = _parse_canonical_ustar_octal(header[124:136], "size")
        data_offset = offset + USTAR_BLOCK_BYTES
        if file_size > archive_size - data_offset:
            raise RegistryV2Error("source archive member size exceeds archive bounds")
        padded_size = ((file_size + USTAR_BLOCK_BYTES - 1) // USTAR_BLOCK_BYTES) * USTAR_BLOCK_BYTES
        if padded_size > archive_size - data_offset:
            raise RegistryV2Error("source archive member padding exceeds archive bounds")
        if not _bytes_are_zero(archive_bytes[data_offset + file_size : data_offset + padded_size]):
            raise RegistryV2Error("source archive member data padding must be zero")

        if path_parts[-1] == b"pafio.toml":
            manifest_count += 1
            if len(path_parts) != 2 or file_size > MAX_ARCHIVE_MANIFEST_BYTES:
                raise RegistryV2Error(
                    "source archive manifest must be exactly <prefix>/pafio.toml "
                    f"and no larger than {MAX_ARCHIVE_MANIFEST_BYTES} bytes"
                )
            manifest_bytes = archive_bytes[data_offset : data_offset + file_size]
        offset = data_offset + padded_size

    if not trailer_seen or offset != archive_size:
        raise RegistryV2Error("source archive must end with exactly two zero blocks")
    if manifest_count != 1:
        raise RegistryV2Error("source archive must contain exactly one <prefix>/pafio.toml")
    return manifest_bytes


def _log_root_hash(leaf_hashes: list[str]) -> str:
    state = bytes(32)
    for leaf_hash in leaf_hashes:
        state = hashlib.sha256(state + bytes.fromhex(leaf_hash)).digest()
    return state.hex()


def _role_keys_dict(role_keys: dict[str, Any]) -> dict[str, Any]:
    return {
        key.keyid: {
            "keytype": "ed25519",
            "scheme": "ed25519",
            "keyval": {
                "public": key.public_key_pem,
            },
        }
        for key in role_keys.values()
    }


def _roles_policy(role_keys: dict[str, Any]) -> dict[str, Any]:
    return {
        "root": {"keyids": [role_keys["root"].keyid], "threshold": 1},
        "timestamp": {"keyids": [role_keys["timestamp"].keyid], "threshold": 1},
        "snapshot": {"keyids": [role_keys["snapshot"].keyid], "threshold": 1},
        "targets": {"keyids": [role_keys["targets"].keyid], "threshold": 1},
        "log": {"keyids": [role_keys["log"].keyid], "threshold": 1},
    }


def _registry_config(
    registry_name: str,
    generated_at: str,
    *,
    publication_id: str | None = None,
    repository_version_id: str | None = None,
) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "schema_version": 1,
        "protocol": "pafio-static-registry",
        "protocol_version": 2,
        "registry_name": registry_name,
        "generated_at": generated_at,
        "capabilities": {
            "append_only_index": True,
            "source_artifacts": True,
            "binary_artifacts": True,
            "transparency_log": True,
        },
        "paths": {
            "root": "trust/root.json",
            "timestamp": "trust/timestamp.json",
            "snapshot": "trust/snapshot.json",
            "targets_prefix": "trust/targets/",
            "index_prefix": "index/",
            "source_artifact_prefix": "artifacts/source/",
            "binary_artifact_prefix": "artifacts/binary/",
            "transparency_checkpoint": "log/checkpoint.json",
            "transparency_leaves_prefix": "log/leaves/",
        },
    }
    if publication_id is not None and repository_version_id is not None:
        payload["repository_id"] = "default"
        payload["distribution_id"] = "default"
        payload["publication"] = {
            "publication_id": publication_id,
            "repository_version_id": repository_version_id,
            "layout_version": 2,
            "publication_path": f"_publications/{publication_id}/publication.json",
            "current_pointer": "_distributions/default/current.json",
        }
    return payload


def _canonical_archive_path(value: str) -> pathlib.Path:
    path = pathlib.Path(value).expanduser()
    if not path.is_absolute():
        path = pathlib.Path.cwd() / path
    return path.resolve()


def _bounded_string(value: Any, context: str, max_bytes: int) -> str:
    text = require_string(value, context)
    if len(text.encode("utf-8")) > max_bytes:
        raise RegistryV2Error(f"{context} exceeds the {max_bytes}-byte limit")
    return text


def _is_safe_package_name(package_name: str) -> bool:
    parts = package_name.split("/")
    if len(parts) != 2:
        return False
    for part in parts:
        if not part or not (("a" <= part[0] <= "z") or ("0" <= part[0] <= "9")):
            return False
        if any(not (("a" <= char <= "z") or ("0" <= char <= "9") or char in "-_") for char in part):
            return False
    return True


def _is_safe_archive_name(archive_name: str) -> bool:
    if archive_name.startswith(".") or not archive_name.endswith(".pafio.src.tar"):
        return False
    return all(
        ("A" <= char <= "Z")
        or ("a" <= char <= "z")
        or ("0" <= char <= "9")
        or char in "-_."
        for char in archive_name
    )


def _is_strict_pafio_version(value: str) -> bool:
    parts = value.split(".")
    return (
        len(parts) == 3
        and all(part and all("0" <= char <= "9" for char in part) for part in parts)
    )


def _normalize_publish_dependencies(entries: Any, field_name: str) -> list[dict[str, str]]:
    if not isinstance(entries, list):
        raise RegistryV2Error(f"{field_name} must be an array")
    if len(entries) > MAX_DEPENDENCIES_PER_TABLE:
        raise RegistryV2Error(f"{field_name} exceeds the {MAX_DEPENDENCIES_PER_TABLE}-entry limit")
    normalized: list[dict[str, str]] = []
    aliases: set[str] = set()
    expected_fields = {"alias", "package", "version_req", "registry"}
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != expected_fields:
            raise RegistryV2Error(f"{field_name} entries must contain exactly alias, package, version_req, and registry")
        alias = _bounded_string(entry["alias"], f"{field_name}.alias", MAX_ALIAS_BYTES)
        package = _bounded_string(entry["package"], f"{field_name}.package", MAX_PACKAGE_BYTES)
        version_req = _bounded_string(entry["version_req"], f"{field_name}.version_req", MAX_VERSION_BYTES)
        registry = _bounded_string(entry["registry"], f"{field_name}.registry", MAX_REGISTRY_BYTES)
        if not _is_safe_package_name(package):
            raise RegistryV2Error(f"{field_name}.package must use lowercase namespace/name form")
        if not _is_strict_pafio_version(version_req):
            raise RegistryV2Error(f"{field_name}.version_req must be strict x.y.z")
        if alias in aliases:
            raise RegistryV2Error(f"{field_name} contains duplicate alias: {alias}")
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


def _release_dependencies(dependencies: list[dict[str, str]], kind: str) -> list[dict[str, Any]]:
    return [
        {
            **dependency,
            "kind": kind,
            "optional": False,
            "target_condition": "",
            "features": [],
        }
        for dependency in dependencies
    ]


def _toml_statements(source: str) -> list[str]:
    statements: list[str] = []
    current: list[str] = []
    brackets: list[str] = []
    state = "normal"
    index = 0
    while index < len(source):
        char = source[index]
        if state == "normal":
            if char == "#":
                newline = source.find("\n", index)
                if newline < 0:
                    break
                if brackets:
                    current.append("\n")
                elif "".join(current).strip():
                    statements.append("".join(current).strip())
                    current = []
                index = newline + 1
                continue
            if source.startswith('"""', index):
                current.append('"""')
                state = "multiline-basic"
                index += 3
                continue
            if source.startswith("'''", index):
                current.append("'''")
                state = "multiline-literal"
                index += 3
                continue
            if char == '"':
                current.append(char)
                state = "basic"
                index += 1
                continue
            if char == "'":
                current.append(char)
                state = "literal"
                index += 1
                continue
            if char in "[{":
                brackets.append(char)
            elif char == "]" and brackets and brackets[-1] == "[":
                brackets.pop()
            elif char == "}" and brackets and brackets[-1] == "{":
                brackets.pop()
            if char == "\n" and not brackets:
                if "".join(current).strip():
                    statements.append("".join(current).strip())
                current = []
            else:
                current.append(char)
            index += 1
            continue
        if state in ("basic", "multiline-basic") and char == "\\":
            current.append(char)
            if index + 1 < len(source):
                current.append(source[index + 1])
                index += 2
            else:
                index += 1
            continue
        if state == "basic" and char == '"':
            current.append(char)
            state = "normal"
            index += 1
            continue
        if state == "literal" and char == "'":
            current.append(char)
            state = "normal"
            index += 1
            continue
        if state == "multiline-basic" and source.startswith('"""', index):
            current.append('"""')
            state = "normal"
            index += 3
            continue
        if state == "multiline-literal" and source.startswith("'''", index):
            current.append("'''")
            state = "normal"
            index += 3
            continue
        current.append(char)
        index += 1
    if "".join(current).strip():
        statements.append("".join(current).strip())
    return statements


_TOML_SHAPE_PROBE = "__pafio_dependency_shape_probe_82da61e4__"


def _find_toml_probe_path(value: Any, path: tuple[str, ...] = ()) -> tuple[str, ...] | None:
    if value == _TOML_SHAPE_PROBE:
        return path
    if isinstance(value, dict):
        for key, child in value.items():
            result = _find_toml_probe_path(child, path + (key,))
            if result is not None:
                return result
    elif isinstance(value, list):
        for child in value:
            result = _find_toml_probe_path(child, path)
            if result is not None:
                return result
    return None


def _toml_header_path(statement: str) -> tuple[str, ...]:
    try:
        probe_document = tomllib.loads(f'{statement}\n{_TOML_SHAPE_PROBE} = "{_TOML_SHAPE_PROBE}"')
    except tomllib.TOMLDecodeError as err:
        raise RegistryV2Error("source package dependency table declaration is not valid TOML") from err
    probe_path = _find_toml_probe_path(probe_document)
    if probe_path is None or probe_path[-1:] != (_TOML_SHAPE_PROBE,):
        raise RegistryV2Error("source package dependency table declaration could not be validated")
    return probe_path[:-1]


def _split_toml_assignment(statement: str) -> tuple[str, str]:
    state = "normal"
    index = 0
    while index < len(statement):
        char = statement[index]
        if state == "normal":
            if char == '"':
                state = "basic"
            elif char == "'":
                state = "literal"
            elif char == "=":
                return statement[:index].strip(), statement[index + 1 :].strip()
        elif state == "basic" and char == "\\":
            index += 1
        elif state == "basic" and char == '"':
            state = "normal"
        elif state == "literal" and char == "'":
            state = "normal"
        index += 1
    raise RegistryV2Error("source package manifest statement must contain a key/value assignment")


def _toml_assignment_path(key_source: str) -> tuple[str, ...]:
    try:
        probe_document = tomllib.loads(f'{key_source} = "{_TOML_SHAPE_PROBE}"')
    except tomllib.TOMLDecodeError as err:
        raise RegistryV2Error("source package dependency key is not valid TOML") from err
    probe_path = _find_toml_probe_path(probe_document)
    if probe_path is None:
        raise RegistryV2Error("source package dependency key could not be validated")
    return probe_path


def _validate_inline_dependency_shape(source: str, manifest_doc: dict[str, Any]) -> None:
    dependency_sections = {"dependencies", "dev-dependencies"}
    inline_aliases = {section: set() for section in dependency_sections}
    active_header: tuple[str, ...] = ()
    for statement in _toml_statements(source):
        stripped = statement.lstrip()
        if stripped.startswith("["):
            active_header = _toml_header_path(statement)
            if active_header and active_header[0] in dependency_sections:
                if stripped.startswith("[[") or len(active_header) != 1:
                    raise RegistryV2Error(
                        f"[{active_header[0]}] entries must be inline tables"
                    )
            continue
        key_source, value_source = _split_toml_assignment(statement)
        absolute_path = active_header + _toml_assignment_path(key_source)
        if not absolute_path or absolute_path[0] not in dependency_sections:
            continue
        if len(absolute_path) != 2 or not value_source.startswith("{"):
            raise RegistryV2Error(f"[{absolute_path[0]}] entries must be inline tables")
        inline_aliases[absolute_path[0]].add(absolute_path[1])

    for section_name in dependency_sections:
        section = manifest_doc.get(section_name, {})
        if isinstance(section, dict) and set(section) != inline_aliases[section_name]:
            raise RegistryV2Error(f"[{section_name}] entries must be inline tables")


def _normalize_dependency(alias: str, spec: Any, section_name: str) -> dict[str, str]:
    alias = _bounded_string(alias, f"dependency alias in [{section_name}]", MAX_ALIAS_BYTES)
    if not isinstance(spec, dict):
        raise RegistryV2Error(f"dependency '{alias}' in [{section_name}] must be an inline table")
    expected_fields = {"package", "version", "registry"}
    if set(spec) != expected_fields:
        raise RegistryV2Error(
            f"dependency '{alias}' in [{section_name}] must contain exactly package, version, and registry"
        )
    package = _bounded_string(
        spec["package"],
        f"dependency '{alias}' package in [{section_name}]",
        MAX_PACKAGE_BYTES,
    )
    version = _bounded_string(
        spec["version"],
        f"dependency '{alias}' version in [{section_name}]",
        MAX_VERSION_BYTES,
    )
    registry = _bounded_string(
        spec["registry"],
        f"dependency '{alias}' registry in [{section_name}]",
        MAX_REGISTRY_BYTES,
    )
    if not _is_safe_package_name(package):
        raise RegistryV2Error(
            f"dependency '{alias}' package in [{section_name}] must use lowercase namespace/name form"
        )
    if not _is_strict_pafio_version(version):
        raise RegistryV2Error(
            f"dependency '{alias}' version in [{section_name}] must be strict x.y.z"
        )
    return {
        "alias": alias,
        "package": package,
        "version_req": version,
        "registry": registry,
    }


def _record_dependencies(manifest_doc: dict[str, Any], section_name: str) -> list[dict[str, str]]:
    section = manifest_doc.get(section_name, {})
    if section is None:
        return []
    if not isinstance(section, dict):
        raise RegistryV2Error(f"[{section_name}] must be a table inside the source package manifest")
    return [_normalize_dependency(alias, spec, section_name) for alias, spec in sorted(section.items())]


def _validate_pafio_manifest(
    manifest_bytes: bytes,
    *,
    expected_package: str,
    expected_version: str,
    expected_dependencies: list[dict[str, str]],
    expected_dev_dependencies: list[dict[str, str]],
) -> None:
    try:
        manifest_source = manifest_bytes.decode("utf-8")
    except UnicodeDecodeError as err:
        raise RegistryV2Error("source package pafio.toml is not UTF-8") from err
    try:
        manifest_doc = tomllib.loads(manifest_source)
    except tomllib.TOMLDecodeError as err:
        raise RegistryV2Error("source package pafio.toml is not valid TOML") from err

    if "spio" in manifest_doc:
        raise RegistryV2Error("source package pafio.toml must not contain [spio]")
    pafio_table = require_object(manifest_doc.get("pafio"), "source package manifest [pafio]")
    manifest_version = pafio_table.get("manifest-version")
    if not isinstance(manifest_version, int) or isinstance(manifest_version, bool) or manifest_version != 1:
        raise RegistryV2Error("source package manifest [pafio].manifest-version must be 1")
    package_table = require_object(manifest_doc.get("package"), "source package manifest [package]")
    package_name = require_string(package_table.get("name"), "source package manifest package.name")
    package_version = require_string(package_table.get("version"), "source package manifest package.version")
    if not _is_strict_pafio_version(package_version):
        raise RegistryV2Error("source package manifest package.version must be strict x.y.z")
    if package_table.get("publish") is not True:
        raise RegistryV2Error("source package manifest package.publish must be true")
    _validate_inline_dependency_shape(manifest_source, manifest_doc)
    dependencies = _record_dependencies(manifest_doc, "dependencies")
    dev_dependencies = _record_dependencies(manifest_doc, "dev-dependencies")
    if package_name != expected_package:
        raise RegistryV2Error("request package does not match archived pafio.toml")
    if package_version != expected_version:
        raise RegistryV2Error("request version does not match archived pafio.toml")
    if dependencies != expected_dependencies:
        raise RegistryV2Error("request dependencies do not match archived pafio.toml")
    if dev_dependencies != expected_dev_dependencies:
        raise RegistryV2Error("request dev_dependencies do not match archived pafio.toml")


def validate_publish_archive_bytes(
    archive_bytes: bytes,
    *,
    package_name: str,
    package_version: str,
    dependencies: list[dict[str, str]],
    dev_dependencies: list[dict[str, str]],
) -> bytes:
    if not isinstance(archive_bytes, bytes):
        raise RegistryV2Error("source archive must be provided as bytes")
    if not archive_bytes or len(archive_bytes) > MAX_ARCHIVE_BYTES:
        raise RegistryV2Error(f"source archive must be between 1 and {MAX_ARCHIVE_BYTES} bytes")
    package_name = _bounded_string(package_name, "package", MAX_PACKAGE_BYTES)
    package_version = _bounded_string(package_version, "version", MAX_VERSION_BYTES)
    if not _is_safe_package_name(package_name):
        raise RegistryV2Error("package must use lowercase namespace/name form")
    if not _is_strict_pafio_version(package_version):
        raise RegistryV2Error("version must be strict x.y.z")
    normalized_dependencies = _normalize_publish_dependencies(dependencies, "dependencies")
    normalized_dev_dependencies = _normalize_publish_dependencies(dev_dependencies, "dev_dependencies")
    manifest_bytes = _validate_deterministic_pafio_ustar(archive_bytes)
    _validate_pafio_manifest(
        manifest_bytes,
        expected_package=package_name,
        expected_version=package_version,
        expected_dependencies=normalized_dependencies,
        expected_dev_dependencies=normalized_dev_dependencies,
    )
    return manifest_bytes


def _extract_record_from_archive_bytes(
    archive_bytes: bytes,
    *,
    expected_package: str,
    expected_version: str,
    expected_dependencies: list[dict[str, str]],
    expected_dev_dependencies: list[dict[str, str]],
    publisher_id: str,
    published_at: str,
) -> dict[str, Any]:
    manifest_bytes = validate_publish_archive_bytes(
        archive_bytes,
        package_name=expected_package,
        package_version=expected_version,
        dependencies=expected_dependencies,
        dev_dependencies=expected_dev_dependencies,
    )
    dependencies = _release_dependencies(expected_dependencies, "runtime")
    dev_dependencies = _release_dependencies(expected_dev_dependencies, "development")
    artifact_sha256 = sha256_bytes(archive_bytes)
    metadata_source = {
        "package": expected_package,
        "version": expected_version,
        "publisher_id": publisher_id,
        "published_at": published_at,
        "archive_sha256": artifact_sha256,
        "dependencies": dependencies,
        "dev_dependencies": dev_dependencies,
    }
    record = {
        "schema_version": 1,
        "package": expected_package,
        "version": expected_version,
        "release_revision": 1,
        "published_at": published_at,
        "publisher_id": publisher_id,
        "yanked": False,
        "deprecated_message": "",
        "source_artifact": {
            "sha256": artifact_sha256,
            "size_bytes": len(archive_bytes),
            "path": artifact_bucket_path("artifacts/source/sha256", artifact_sha256, ".pafio.src.tar").as_posix(),
            "archive_format": "tar",
            "compression": "none",
        },
        "binary_artifacts": [],
        "dependencies": dependencies,
        "dev_dependencies": dev_dependencies,
        "features": {
            "default": [],
            "optional": [],
        },
        "manifest_digest": sha256_bytes(manifest_bytes),
        "metadata_digest": sha256_bytes(canonical_json_bytes(metadata_source)),
    }
    return record


def _read_signed_version(path: pathlib.Path) -> int:
    if not path.exists():
        return 0
    payload = load_json_file(path, f"signed metadata {path.name}")
    signed = require_object(payload.get("signed"), f"signed metadata {path.name}.signed")
    version = signed.get("version")
    if not isinstance(version, int):
        raise RegistryV2Error(f"signed metadata version must be an integer: {path}")
    return version


def _leaf_sequence_paths(dest_root: pathlib.Path) -> list[pathlib.Path]:
    leaves_root = dest_root / "log" / "leaves"
    if not leaves_root.exists():
        return []
    paths = sorted(path for path in leaves_root.glob("*.json") if path.is_file())
    for index, path in enumerate(paths, start=1):
        expected = f"{index:012d}.json"
        if path.name != expected:
            raise RegistryV2Error(f"registry v2 leaves must remain contiguous; expected {expected} but found {path.name}")
    return paths


def _ensure_root_matches_keys(dest_root: pathlib.Path, role_keys: dict[str, Any]) -> None:
    root_path = dest_root / "trust" / "root.json"
    if not root_path.exists():
        return
    root_payload = load_json_file(root_path, "registry v2 root metadata")
    signed = require_object(root_payload.get("signed"), "registry v2 root metadata signed payload")
    roles = require_object(signed.get("roles"), "registry v2 root metadata roles")
    for role_name, role_key in role_keys.items():
        if role_name not in roles:
            raise RegistryV2Error(f"registry v2 root metadata is missing role '{role_name}'")
        role_policy = require_object(roles.get(role_name), f"registry v2 root metadata role '{role_name}'")
        keyids = role_policy.get("keyids")
        if keyids != [role_key.keyid]:
            raise RegistryV2Error(
                f"registry v2 root metadata key id for role '{role_name}' does not match the provided key directory"
            )


def _initialize_registry_root(
    dest_root: pathlib.Path,
    role_keys: dict[str, Any],
    *,
    registry_name: str,
    registry_time: str,
) -> bool:
    config_path = dest_root / "config.json"
    root_path = dest_root / "trust" / "root.json"
    if config_path.exists() != root_path.exists():
        raise RegistryV2Error("registry v2 root is only partially initialized; config.json and trust/root.json must either both exist or both be absent")
    if config_path.exists() or root_path.exists():
        _ensure_root_matches_keys(dest_root, role_keys)
        config = load_json_file(config_path, "registry v2 config")
        if config.get("protocol") != "pafio-static-registry" or config.get("protocol_version") != 2:
            raise RegistryV2Error("existing registry root does not match the registry v2 protocol")
        existing_name = require_string(config.get("registry_name"), "registry v2 config registry_name")
        if existing_name != registry_name:
            raise RegistryV2Error(
                f"existing registry v2 root is named '{existing_name}', but the publish request expected '{registry_name}'"
            )
        return False

    dest_root.mkdir(parents=True, exist_ok=True)
    write_json_file(config_path, _registry_config(registry_name, registry_time))

    checkpoint_signed = {
        "type": "checkpoint",
        "spec_version": "1",
        "version": 1,
        "generated_at": registry_time,
        "tree_size": 0,
        "root_hash": _log_root_hash([]),
    }
    checkpoint_envelope = sign_payload(checkpoint_signed, role_keys["log"])
    checkpoint_path = dest_root / "log" / "checkpoint.json"
    write_json_file(checkpoint_path, checkpoint_envelope)

    snapshot_signed = {
        "type": "snapshot",
        "spec_version": "1",
        "version": 1,
        "expires": expires_in(7),
        "meta": {},
        "log_meta": {
            "log/checkpoint.json": signed_file_meta(checkpoint_path, version=1),
        },
    }
    snapshot_envelope = sign_payload(snapshot_signed, role_keys["snapshot"])
    snapshot_path = dest_root / "trust" / "snapshot.json"
    write_json_file(snapshot_path, snapshot_envelope)

    timestamp_signed = {
        "type": "timestamp",
        "spec_version": "1",
        "version": 1,
        "expires": expires_in(1),
        "meta": {
            "trust/snapshot.json": signed_file_meta(snapshot_path, version=1),
        },
    }
    timestamp_envelope = sign_payload(timestamp_signed, role_keys["timestamp"])
    timestamp_path = dest_root / "trust" / "timestamp.json"
    write_json_file(timestamp_path, timestamp_envelope)

    root_signed = {
        "type": "root",
        "spec_version": "1",
        "version": 1,
        "expires": expires_in(365),
        "keys": _role_keys_dict(role_keys),
        "roles": _roles_policy(role_keys),
    }
    root_envelope = sign_payload(root_signed, role_keys["root"])
    write_json_file(root_path, root_envelope)
    return True


def _collect_package_maps(dest_root: pathlib.Path) -> tuple[dict[str, dict[str, Any]], dict[str, Any]]:
    index_root = dest_root / "index"
    namespace_packages: dict[str, dict[str, Any]] = defaultdict(dict)
    snapshot_meta: dict[str, Any] = {}
    if not index_root.exists():
        return namespace_packages, snapshot_meta

    for index_path in sorted(path for path in index_root.rglob("*.jsonl") if path.is_file()):
        lines = [line for line in index_path.read_text(encoding="utf-8").splitlines() if line.strip()]
        if not lines:
            raise RegistryV2Error(f"registry v2 index file is empty: {index_path}")
        records: list[dict[str, Any]] = []
        for line in lines:
            payload = json.loads(line)
            records.append(require_object(payload, f"registry v2 index record in {index_path}"))
        package_name = require_string(records[0].get("package"), f"registry v2 first package name in {index_path}")
        namespace, _ = split_package_name(package_name)
        versions = []
        releases: dict[str, Any] = {}
        for record in records:
            if require_string(record.get("package"), f"registry v2 package name in {index_path}") != package_name:
                raise RegistryV2Error(f"registry v2 index file contains mixed package names: {index_path}")
            version = require_string(record.get("version"), f"registry v2 version in {index_path}")
            source_artifact = require_object(record.get("source_artifact"), f"registry v2 source artifact in {index_path}")
            versions.append(version)
            releases[version] = {
                "release_revision": record.get("release_revision", 1),
                "index_record_sha256": sha256_bytes(canonical_json_bytes(record)),
                "source_artifact_sha256": require_string(
                    source_artifact.get("sha256"),
                    f"registry v2 source artifact sha256 in {index_path}",
                ),
                "source_artifact_path": require_string(
                    source_artifact.get("path"),
                    f"registry v2 source artifact path in {index_path}",
                ),
                "binary_artifact_count": len(record.get("binary_artifacts", [])),
            }
        namespace_packages[namespace][package_name] = {
            "index_path": index_path.relative_to(dest_root).as_posix(),
            "latest_version": sorted(versions, key=parse_semver_key)[-1],
            "releases": releases,
        }
        snapshot_meta[index_path.relative_to(dest_root).as_posix()] = signed_file_meta(index_path, version=1)
    return namespace_packages, snapshot_meta


def _refresh_signed_metadata(dest_root: pathlib.Path, role_keys: dict[str, Any], registry_time: str) -> dict[str, int]:
    namespace_packages, snapshot_meta = _collect_package_maps(dest_root)

    for namespace, package_map in namespace_packages.items():
        targets_path = dest_root / "trust" / "targets" / f"{namespace}.json"
        targets_version = _read_signed_version(targets_path) + 1
        targets_signed = {
            "type": "targets",
            "spec_version": "1",
            "version": targets_version,
            "expires": expires_in(30),
            "namespace": namespace,
            "packages": package_map,
        }
        write_json_file(targets_path, sign_payload(targets_signed, role_keys["targets"]))
        snapshot_meta[targets_path.relative_to(dest_root).as_posix()] = signed_file_meta(targets_path, version=targets_version)

    checkpoint_path = dest_root / "log" / "checkpoint.json"
    checkpoint_version = _read_signed_version(checkpoint_path) + 1
    leaf_hashes = [
        sha256_bytes(canonical_json_bytes(load_json_file(path, f"registry v2 log leaf {path.name}")))
        for path in _leaf_sequence_paths(dest_root)
    ]
    checkpoint_signed = {
        "type": "checkpoint",
        "spec_version": "1",
        "version": checkpoint_version,
        "generated_at": registry_time,
        "tree_size": len(leaf_hashes),
        "root_hash": _log_root_hash(leaf_hashes),
    }
    write_json_file(checkpoint_path, sign_payload(checkpoint_signed, role_keys["log"]))

    snapshot_path = dest_root / "trust" / "snapshot.json"
    snapshot_version = _read_signed_version(snapshot_path) + 1
    snapshot_signed = {
        "type": "snapshot",
        "spec_version": "1",
        "version": snapshot_version,
        "expires": expires_in(7),
        "meta": snapshot_meta,
        "log_meta": {
            "log/checkpoint.json": signed_file_meta(checkpoint_path, version=checkpoint_version),
        },
    }
    write_json_file(snapshot_path, sign_payload(snapshot_signed, role_keys["snapshot"]))

    timestamp_path = dest_root / "trust" / "timestamp.json"
    timestamp_version = _read_signed_version(timestamp_path) + 1
    timestamp_signed = {
        "type": "timestamp",
        "spec_version": "1",
        "version": timestamp_version,
        "expires": expires_in(1),
        "meta": {
            "trust/snapshot.json": signed_file_meta(snapshot_path, version=snapshot_version),
        },
    }
    write_json_file(timestamp_path, sign_payload(timestamp_signed, role_keys["timestamp"]))

    return {
        "checkpoint_version": checkpoint_version,
        "snapshot_version": snapshot_version,
        "timestamp_version": timestamp_version,
        "namespaces": len(namespace_packages),
    }


def _directory_digest(root: pathlib.Path) -> str:
    files: list[dict[str, str]] = []
    if root.exists():
        for path in sorted(candidate for candidate in root.rglob("*") if candidate.is_file()):
            files.append(
                {
                    "path": path.relative_to(root).as_posix(),
                    "sha256": sha256_file(path),
                }
            )
    return "sha256:" + sha256_bytes(canonical_json_bytes(files))


def _next_publication_sequence(dest_root: pathlib.Path) -> int:
    sequence = 1
    publications_root = dest_root / "_publications"
    if not publications_root.exists():
        return sequence
    for path in publications_root.iterdir():
        if not path.is_dir() or not path.name.startswith("pub-"):
            continue
        try:
            sequence = max(sequence, int(path.name.removeprefix("pub-")) + 1)
        except ValueError:
            continue
    return sequence


def _copy_read_plane(source_root: pathlib.Path, target_root: pathlib.Path) -> None:
    target_root.mkdir(parents=True, exist_ok=True)
    for name in ("index", "artifacts", "trust", "log"):
        source = source_root / name
        if source.exists():
            shutil.copytree(source, target_root / name, dirs_exist_ok=True)
    shutil.copy2(source_root / "config.json", target_root / "config.json")


def _create_publication(dest_root: pathlib.Path, registry_name: str, registry_time: str) -> dict[str, Any]:
    sequence = _next_publication_sequence(dest_root)
    publication_id = f"pub-{sequence:06d}"
    repository_version_id = f"rv-{sequence:06d}"
    temp_root = dest_root / "_tmp" / "publications" / publication_id
    final_root = dest_root / "_publications" / publication_id
    if final_root.exists():
        raise RegistryV2Error(f"publication already exists: {publication_id}")
    if temp_root.exists():
        shutil.rmtree(temp_root)
    _copy_read_plane(dest_root, temp_root)
    write_json_file(
        temp_root / "config.json",
        _registry_config(
            registry_name,
            registry_time,
            publication_id=publication_id,
            repository_version_id=repository_version_id,
        ),
    )
    tree_size = len(_leaf_sequence_paths(temp_root))
    artifact_count = sum(1 for path in (temp_root / "artifacts").rglob("*") if path.is_file()) if (temp_root / "artifacts").exists() else 0
    publication = {
        "publication_id": publication_id,
        "repository_id": "default",
        "repository_version_id": repository_version_id,
        "layout_version": 2,
        "generated_at": registry_time,
        "tree_size": tree_size,
        "index_digest": _directory_digest(temp_root / "index"),
        "trust_digest": _directory_digest(temp_root / "trust"),
        "artifact_count": artifact_count,
        "verified": True,
    }
    write_json_file(temp_root / "publication.json", publication)
    if not (temp_root / "config.json").exists() or not (temp_root / "trust" / "root.json").exists():
        raise RegistryV2Error("publication verification failed before distribution pointer update")
    final_root.parent.mkdir(parents=True, exist_ok=True)
    temp_root.rename(final_root)
    shutil.copy2(final_root / "config.json", dest_root / "config.json")

    current_path = dest_root / "_distributions" / "default" / "current.json"
    previous_publication_id = ""
    if current_path.exists():
        previous = load_json_file(current_path, "registry v2 distribution current pointer")
        previous_publication_id = str(previous.get("publication_id", ""))
    current = {
        "distribution_id": "default",
        "publication_id": publication_id,
        "repository_version_id": repository_version_id,
        "updated_at": registry_time,
    }
    if previous_publication_id:
        current["previous_publication_id"] = previous_publication_id
    write_json_file(current_path, current)

    publication["manifest_sha256"] = sha256_file(final_root / "publication.json")
    publication["root_path"] = final_root.as_posix()
    publication["distribution"] = current
    return publication


def _append_release_record(dest_root: pathlib.Path, record: dict[str, Any], archive_bytes: bytes) -> dict[str, Any]:
    package_name = require_string(record.get("package"), "registry v2 publish record package")
    package_version = require_string(record.get("version"), "registry v2 publish record version")
    artifact = require_object(record.get("source_artifact"), "registry v2 publish source artifact")
    artifact_relative_path = require_string(artifact.get("path"), "registry v2 publish source artifact path")
    artifact_dest_path = dest_root / pathlib.PurePosixPath(artifact_relative_path)
    artifact_sha256 = require_string(artifact.get("sha256"), "registry v2 publish source artifact sha256")
    if artifact_dest_path.exists():
        if (
            not artifact_dest_path.is_file()
            or artifact_dest_path.stat().st_size != len(archive_bytes)
            or sha256_file(artifact_dest_path) != artifact_sha256
        ):
            raise RegistryV2Error("destination source artifact already exists with different content")
    else:
        atomic_write_bytes(artifact_dest_path, archive_bytes)

    index_path = dest_root / index_path_for_package(package_name)
    existing_records: list[dict[str, Any]] = []
    if index_path.exists():
        existing_lines = [line for line in index_path.read_text(encoding="utf-8").splitlines() if line.strip()]
        for line in existing_lines:
            payload = json.loads(line)
            existing_records.append(require_object(payload, f"registry v2 existing index record in {index_path}"))
        for payload in existing_records:
            if require_string(payload.get("version"), f"registry v2 existing version in {index_path}") == package_version:
                raise RegistryV2Error(f"package version is already published in registry v2: {package_name}@{package_version}")
    ensure_parent(index_path)
    with index_path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(record, sort_keys=True, ensure_ascii=False) + "\n")

    leaf_paths = _leaf_sequence_paths(dest_root)
    sequence = len(leaf_paths) + 1
    namespace, _ = split_package_name(package_name)
    leaf = {
        "schema_version": 1,
        "sequence": sequence,
        "namespace": namespace,
        "package": package_name,
        "version": package_version,
        "release_revision": 1,
        "index_path": index_path.relative_to(dest_root).as_posix(),
        "index_record_sha256": sha256_bytes(canonical_json_bytes(record)),
        "source_artifact_sha256": require_string(artifact.get("sha256"), "registry v2 publish source artifact sha256"),
        "source_artifact_path": artifact_relative_path,
    }
    leaf_path = dest_root / "log" / "leaves" / f"{sequence:012d}.json"
    write_json_file(leaf_path, leaf)
    return {
        "index_path": index_path,
        "leaf_path": leaf_path,
        "sequence": sequence,
    }


def publish_to_registry_v2(
    dest_root_value: str,
    key_dir_value: str,
    *,
    archive_path_value: str,
    archive_name: str,
    package_name: str,
    package_version: str,
    dependencies: list[dict[str, str]],
    dev_dependencies: list[dict[str, str]],
    registry_name: str = "pafio-static-registry",
    publisher_id: str = "control-plane",
) -> dict[str, Any]:
    package_name = _bounded_string(package_name, "package", MAX_PACKAGE_BYTES)
    package_version = _bounded_string(package_version, "version", MAX_VERSION_BYTES)
    archive_name = _bounded_string(archive_name, "archive_name", MAX_ARCHIVE_NAME_BYTES)
    publisher_id = _bounded_string(publisher_id, "publisher_id", MAX_PACKAGE_BYTES)
    if not _is_safe_package_name(package_name):
        raise RegistryV2Error("package must use lowercase namespace/name form")
    if not _is_strict_pafio_version(package_version):
        raise RegistryV2Error("version must be strict x.y.z")
    if not _is_safe_archive_name(archive_name):
        raise RegistryV2Error("archive_name must be a safe basename ending in .pafio.src.tar")
    normalized_dependencies = _normalize_publish_dependencies(dependencies, "dependencies")
    normalized_dev_dependencies = _normalize_publish_dependencies(dev_dependencies, "dev_dependencies")
    archive_path = _canonical_archive_path(archive_path_value)
    if not archive_path.is_file():
        raise RegistryV2Error("registry v2 publish archive was not found")
    try:
        archive_bytes = archive_path.read_bytes()
    except OSError as err:
        raise RegistryV2Error("registry v2 publish archive could not be read") from err
    if not archive_bytes or len(archive_bytes) > MAX_ARCHIVE_BYTES:
        raise RegistryV2Error(f"registry v2 publish archive must be between 1 and {MAX_ARCHIVE_BYTES} bytes")

    registry_time = utc_now().strftime("%Y-%m-%dT%H:%M:%SZ")
    record = _extract_record_from_archive_bytes(
        archive_bytes,
        expected_package=package_name,
        expected_version=package_version,
        expected_dependencies=normalized_dependencies,
        expected_dev_dependencies=normalized_dev_dependencies,
        publisher_id=publisher_id,
        published_at=registry_time,
    )
    dest_root = normalize_local_root(dest_root_value)
    key_dir = normalize_local_root(key_dir_value)
    role_keys = load_role_keys(key_dir)
    created_root = _initialize_registry_root(dest_root, role_keys, registry_name=registry_name, registry_time=registry_time)
    append_result = _append_release_record(dest_root, record, archive_bytes)
    metadata_versions = _refresh_signed_metadata(dest_root, role_keys, registry_time)
    publication = _create_publication(dest_root, registry_name, registry_time)

    return {
        "created_root": created_root,
        "package": record["package"],
        "version": record["version"],
        "publisher_id": publisher_id,
        "published_at": registry_time,
        "archive_name": archive_name,
        "archive_sha256": record["source_artifact"]["sha256"],
        "archive_size_bytes": record["source_artifact"]["size_bytes"],
        "artifact_path": record["source_artifact"]["path"],
        "index_path": append_result["index_path"].relative_to(dest_root).as_posix(),
        "log_leaf_path": append_result["leaf_path"].relative_to(dest_root).as_posix(),
        "sequence": append_result["sequence"],
        "dependencies": normalized_dependencies,
        "dev_dependencies": normalized_dev_dependencies,
        "checkpoint_version": metadata_versions["checkpoint_version"],
        "snapshot_version": metadata_versions["snapshot_version"],
        "timestamp_version": metadata_versions["timestamp_version"],
        "namespaces": metadata_versions["namespaces"],
        "repository_id": "default",
        "repository_version_id": publication["repository_version_id"],
        "publication_id": publication["publication_id"],
        "distribution_id": "default",
    }


def initialize_registry_v2_root(
    dest_root_value: str,
    key_dir_value: str,
    *,
    registry_name: str = "pafio-static-registry",
) -> dict[str, Any]:
    from .keygen import generate_key_directory

    dest_root = normalize_local_root(dest_root_value)
    key_dir = normalize_local_root(key_dir_value)
    if not (key_dir / "keys.json").exists():
        generate_key_directory(key_dir)
    role_keys = load_role_keys(key_dir)
    registry_time = utc_now().strftime("%Y-%m-%dT%H:%M:%SZ")
    created_root = _initialize_registry_root(dest_root, role_keys, registry_name=registry_name, registry_time=registry_time)
    return {
        "ok": True,
        "registry_root": str(dest_root),
        "key_dir": str(key_dir),
        "registry_name": registry_name,
        "created_root": created_root,
    }
