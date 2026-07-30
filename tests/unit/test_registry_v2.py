from __future__ import annotations

import json
import pathlib
import runpy
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
PACKAGE_REGISTRY = ROOT / "src" / "PlatformCloud" / "PackageRegistry"
if str(PACKAGE_REGISTRY) not in sys.path:
    sys.path.insert(0, str(PACKAGE_REGISTRY))

import package_registry_v2.common as common  # noqa: E402
from package_registry_v2 import generate_key_directory, publish_to_registry_v2, verify_registry_root  # noqa: E402
from package_registry_v2.common import RegistryV2Error, sha256_file  # noqa: E402
from package_registry_v2 import validator  # noqa: E402
from package_registry_v2.validator import RootReader  # noqa: E402


CONTRACT_DIR = ROOT / "contracts" / "registry-v2" / "v1"


def ustar_member(
    path: str,
    content: bytes,
    *,
    mode: int = 0o644,
    typeflag: bytes = b"0",
    linkname: str = "",
) -> tuple[str, bytes, int, bytes, str]:
    return path, content, mode, typeflag, linkname


def ustar_octal(value: int, width: int) -> bytes:
    encoded = f"{value:0{width - 1}o}".encode("ascii") + b"\0"
    if len(encoded) != width:
        raise ValueError("ustar fixture numeric field overflow")
    return encoded


def deterministic_pafio_ustar(members: list[tuple[str, bytes, int, bytes, str]]) -> bytes:
    archive = bytearray()
    for path, content, mode, typeflag, linkname in members:
        name = path.encode("utf-8")
        link = linkname.encode("utf-8")
        if len(name) > 100 or len(link) > 100:
            raise ValueError("test ustar path exceeds the direct name/link field")
        header = bytearray(512)
        header[0 : len(name)] = name
        header[100:108] = ustar_octal(mode, 8)
        header[108:116] = ustar_octal(0, 8)
        header[116:124] = ustar_octal(0, 8)
        header[124:136] = ustar_octal(len(content), 12)
        header[136:148] = ustar_octal(0, 12)
        header[148:156] = b"        "
        header[156:157] = typeflag
        header[157 : 157 + len(link)] = link
        header[257:263] = b"ustar\0"
        header[263:265] = b"00"
        checksum = sum(header)
        header[148:156] = f"{checksum:06o}\0 ".encode("ascii")
        archive.extend(header)
        archive.extend(content)
        archive.extend(b"\0" * ((-len(content)) % 512))
    archive.extend(b"\0" * 1024)
    return bytes(archive)


def pafio_manifest(
    package_name: str = "acme/util",
    version: str = "1.0.0",
    *,
    publish: bool = True,
    manifest_version: int = 1,
    preamble: str = "",
    dependency_source: str = "",
) -> bytes:
    return (
        f"""{preamble}[pafio]
manifest-version = {manifest_version}

[package]
name = "{package_name}"
version = "{version}"
edition = "2026"
publish = {"true" if publish else "false"}

[build]
implicit-std = true

[lib]
path = "src/lib.styio"
{dependency_source}
"""
    ).encode("utf-8")


class RegistryV2Tests(unittest.TestCase):
    def _build_source_archive(
        self,
        package_name: str,
        version: str,
        destination: pathlib.Path,
        *,
        manifest: bytes | None = None,
    ) -> str:
        short_name = package_name.split("/", 1)[1]
        prefix = f"{short_name}-{version}"
        archive_bytes = deterministic_pafio_ustar(
            [
                ustar_member(f"{prefix}/pafio.toml", manifest or pafio_manifest(package_name, version)),
                ustar_member(f"{prefix}/src/lib.styio", f"# {package_name}@{version}\n".encode("utf-8")),
            ]
        )
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(archive_bytes)
        return sha256_file(destination)

    def _publish_archive(
        self,
        dest_root: pathlib.Path,
        key_dir: pathlib.Path,
        archive: pathlib.Path,
        package: str,
        version: str,
        *,
        registry_name: str = "test-registry",
        dependencies: list[dict[str, str]] | None = None,
        dev_dependencies: list[dict[str, str]] | None = None,
        archive_name: str | None = None,
    ) -> dict[str, object]:
        return publish_to_registry_v2(
            str(dest_root),
            str(key_dir),
            archive_path_value=str(archive),
            archive_name=archive_name or f"{package.replace('/', '-')}-{version}.pafio.src.tar",
            package_name=package,
            package_version=version,
            dependencies=dependencies or [],
            dev_dependencies=dev_dependencies or [],
            registry_name=registry_name,
            publisher_id="control-plane",
        )

    def test_contract_pack_inventory(self) -> None:
        contract = json.loads((CONTRACT_DIR / "registry-v2.contract.json").read_text(encoding="utf-8"))
        self.assertEqual(contract["protocol"], "pafio-static-registry")
        self.assertEqual(contract["protocol_version"], 2)
        for schema_name, relative_path in contract["schemas"].items():
            with self.subTest(schema=schema_name):
                schema_path = CONTRACT_DIR / relative_path
                self.assertTrue(schema_path.exists(), f"missing schema: {schema_path}")

        examples = json.loads((CONTRACT_DIR / "registry-v2.examples.json").read_text(encoding="utf-8"))
        self.assertIn("config", examples)
        self.assertIn("package_index_record", examples)
        self.assertIn("transparency_log_leaf", examples)
        self.assertEqual(examples["config"]["protocol_version"], 2)
        self.assertEqual(examples["package_index_record"]["package"], "acme/util")

    def test_publish_and_verify_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            dest_root = root / "registry-v2"
            key_dir = root / "keys"
            archive_root = root / "artifacts"
            archive_one = archive_root / "util-1.0.0.tar"
            archive_two = archive_root / "util-1.2.0.tar"
            self._build_source_archive("acme/util", "1.0.0", archive_one)
            self._build_source_archive("acme/util", "1.2.0", archive_two)

            manifest = generate_key_directory(key_dir)
            self.assertEqual(manifest["algorithm"], "ed25519")

            self._publish_archive(dest_root, key_dir, archive_one, "acme/util", "1.0.0")
            self._publish_archive(dest_root, key_dir, archive_two, "acme/util", "1.2.0")

            verified = verify_registry_root(str(dest_root))
            self.assertTrue(verified["ok"])
            self.assertEqual(verified["namespaces"], 1)
            self.assertEqual(verified["index_files"], 1)
            self.assertEqual(verified["releases"], 2)
            self.assertEqual(verified["tree_size"], 2)

            lines = (dest_root / "index" / "acme" / "util.jsonl").read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(lines), 2)
            latest = json.loads(lines[-1])
            self.assertEqual(latest["version"], "1.2.0")
            self.assertEqual(latest["source_artifact"]["compression"], "none")

    def test_reader_enforces_http_timeout(self) -> None:
        original_timeout = validator.HTTP_READ_TIMEOUT_SECONDS
        original_urlopen = validator.urlopen
        observed: dict[str, object] = {}

        def fake_urlopen(location: str, timeout: float | None = None):
            observed["location"] = location
            observed["timeout"] = timeout
            raise TimeoutError("timed out")

        try:
            validator.HTTP_READ_TIMEOUT_SECONDS = 0.25
            validator.urlopen = fake_urlopen
            reader = RootReader("https://packages.example.test/pafio/")
            with self.assertRaises(RegistryV2Error):
                reader.read_bytes("config.json")
        finally:
            validator.urlopen = original_urlopen
            validator.HTTP_READ_TIMEOUT_SECONDS = original_timeout
        self.assertEqual(observed["location"], "https://packages.example.test/pafio/config.json")
        self.assertEqual(observed["timeout"], 0.25)

    def test_reader_rejects_oversized_remote_metadata(self) -> None:
        original_max = validator.HTTP_METADATA_MAX_BYTES
        original_urlopen = validator.urlopen

        class FakeResponse:
            headers = {"Content-Type": "application/json"}

            def __enter__(self) -> "FakeResponse":
                return self

            def __exit__(self, *args: object) -> None:
                return None

            def read(self, length: int = -1) -> bytes:
                return b"x" * length

        try:
            validator.HTTP_METADATA_MAX_BYTES = 8
            validator.urlopen = lambda location, timeout=None: FakeResponse()
            reader = RootReader("https://packages.example.test/pafio/")
            with self.assertRaises(RegistryV2Error):
                reader.read_bytes("config.json")
        finally:
            validator.urlopen = original_urlopen
            validator.HTTP_METADATA_MAX_BYTES = original_max

    def test_reader_rejects_unexpected_remote_metadata_media_type(self) -> None:
        original_urlopen = validator.urlopen

        class FakeResponse:
            headers = {"Content-Type": "text/html; charset=utf-8"}

            def __enter__(self) -> "FakeResponse":
                return self

            def __exit__(self, *args: object) -> None:
                return None

            def read(self, length: int = -1) -> bytes:
                return b"{}"

        try:
            validator.urlopen = lambda location, timeout=None: FakeResponse()
            reader = RootReader("https://packages.example.test/pafio/")
            with self.assertRaises(RegistryV2Error):
                reader.read_bytes("config.json")
        finally:
            validator.urlopen = original_urlopen

    def test_control_plane_request_timeout_is_reported(self) -> None:
        module = runpy.run_path(str(ROOT / "scripts" / "registry-v2-control-plane-server.py"))
        load_json_request = module["load_json_request"]

        class FakeBody:
            def read(self, length: int) -> bytes:
                raise TimeoutError("timed out")

        handler = type(
            "Handler",
            (),
            {
                "headers": {"Content-Length": "3"},
                "rfile": FakeBody(),
            },
        )()

        with self.assertRaises(ValueError) as ctx:
            load_json_request(handler)
        self.assertIn("timed out after", str(ctx.exception))

    def test_openssl_run_enforces_subprocess_timeout(self) -> None:
        original_run = common.subprocess.run
        observed: dict[str, object] = {}

        def fake_run(command: list[str], **kwargs: object):
            observed["command"] = command
            observed["timeout"] = kwargs.get("timeout")
            raise subprocess.TimeoutExpired(command, kwargs.get("timeout"))

        try:
            common.subprocess.run = fake_run
            with self.assertRaises(RegistryV2Error) as ctx:
                common.openssl_run(["version"])
        finally:
            common.subprocess.run = original_run

        self.assertIn("timed out after", str(ctx.exception))
        self.assertEqual(observed["command"], ["openssl", "version"])
        self.assertEqual(observed["timeout"], common.REGISTRY_OPENSSL_TIMEOUT_SECONDS)

    def test_publish_rejects_request_metadata_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            dest_root = root / "registry-v2"
            key_dir = root / "keys"
            archive = root / "artifacts" / "util-1.0.0.tar"
            self._build_source_archive("acme/util", "1.0.0", archive)
            generate_key_directory(key_dir)

            with self.assertRaises(RegistryV2Error):
                self._publish_archive(dest_root, key_dir, archive, "acme/other", "1.0.0")
            self.assertFalse((dest_root / "config.json").exists())

    def test_publish_rejects_request_dependency_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            dest_root = root / "registry-v2"
            key_dir = root / "keys"
            archive = root / "artifacts" / "util-1.0.0.tar"
            self._build_source_archive("acme/util", "1.0.0", archive)
            generate_key_directory(key_dir)
            unexpected_dependency = {
                "alias": "base",
                "package": "acme/base",
                "version_req": "1.0.0",
                "registry": "https://packages.example.test",
            }

            for field_name in ("dependencies", "dev_dependencies"):
                with self.subTest(field=field_name):
                    arguments = {field_name: [unexpected_dependency]}
                    with self.assertRaises(RegistryV2Error):
                        self._publish_archive(
                            dest_root,
                            key_dir,
                            archive,
                            "acme/util",
                            "1.0.0",
                            **arguments,
                        )
                    self.assertFalse((dest_root / "config.json").exists())

    def test_publish_rejects_noncanonical_ustar_without_registry_or_key_state(self) -> None:
        manifest = pafio_manifest()
        canonical = deterministic_pafio_ustar(
            [
                ustar_member("util-1.0.0/pafio.toml", manifest),
                ustar_member("util-1.0.0/src/lib.styio", b"# util\n"),
            ]
        )
        bad_checksum = bytearray(canonical)
        bad_checksum[0] ^= 1
        bad_padding = bytearray(canonical)
        bad_padding[512 + len(manifest)] = 1
        invalid_archives = {
            "backslash": deterministic_pafio_ustar(
                [ustar_member("util-1.0.0\\pafio.toml", manifest)]
            ),
            "traversal": deterministic_pafio_ustar(
                [ustar_member("util-1.0.0/../pafio.toml", manifest)]
            ),
            "symlink": deterministic_pafio_ustar(
                [
                    ustar_member("util-1.0.0/pafio.toml", manifest),
                    ustar_member("util-1.0.0/src/lib.styio", b"# util\n"),
                    ustar_member(
                        "util-1.0.0/z-link",
                        b"",
                        typeflag=b"2",
                        linkname="src/lib.styio",
                    ),
                ]
            ),
            "duplicate": deterministic_pafio_ustar(
                [
                    ustar_member("util-1.0.0/pafio.toml", manifest),
                    ustar_member("util-1.0.0/pafio.toml", manifest),
                ]
            ),
            "multiple-prefixes": deterministic_pafio_ustar(
                [
                    ustar_member("alpha/pafio.toml", manifest),
                    ustar_member("beta/src/lib.styio", b"# util\n"),
                ]
            ),
            "multiple-manifests": deterministic_pafio_ustar(
                [
                    ustar_member("util-1.0.0/pafio.toml", manifest),
                    ustar_member("util-1.0.0/sub/pafio.toml", manifest),
                ]
            ),
            "checksum": bytes(bad_checksum),
            "trailer": canonical + b"\0" * 512,
            "padding": bytes(bad_padding),
            "order": deterministic_pafio_ustar(
                [
                    ustar_member("util-1.0.0/src/lib.styio", b"# util\n"),
                    ustar_member("util-1.0.0/pafio.toml", manifest),
                ]
            ),
            "mode": deterministic_pafio_ustar(
                [
                    ustar_member("util-1.0.0/pafio.toml", manifest, mode=0o755),
                    ustar_member("util-1.0.0/src/lib.styio", b"# util\n"),
                ]
            ),
        }

        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            for name, archive_bytes in invalid_archives.items():
                with self.subTest(case=name):
                    archive = root / "inputs" / f"{name}.pafio.src.tar"
                    archive.parent.mkdir(parents=True, exist_ok=True)
                    archive.write_bytes(archive_bytes)
                    dest_root = root / name / "registry-v2"
                    key_dir = root / name / "keys"
                    with self.assertRaises(RegistryV2Error):
                        self._publish_archive(dest_root, key_dir, archive, "acme/util", "1.0.0")
                    self.assertFalse(dest_root.exists())
                    self.assertFalse(key_dir.exists())

    def test_publish_rejects_invalid_pafio_manifest_without_registry_or_key_state(self) -> None:
        exact_dependency = {
            "alias": "alpha",
            "package": "acme/alpha",
            "version_req": "1.0.0",
            "registry": "https://packages.example.test",
        }
        inline_exact = """

[dependencies]
alpha = { package = "acme/alpha", version = "1.0.0", registry = "https://packages.example.test" }
"""
        invalid_manifests = {
            "legacy-spio": (pafio_manifest(preamble="[spio]\nlegacy = true\n\n"), "acme/util", "1.0.0", []),
            "manifest-version": (
                pafio_manifest(manifest_version=2),
                "acme/util",
                "1.0.0",
                [],
            ),
            "publish-false": (
                pafio_manifest(publish=False),
                "acme/util",
                "1.0.0",
                [],
            ),
            "package-identity": (
                pafio_manifest(package_name="acme/other"),
                "acme/util",
                "1.0.0",
                [],
            ),
            "version-mismatch": (
                pafio_manifest(version="1.0.1"),
                "acme/util",
                "1.0.0",
                [],
            ),
            "manifest-version-shape": (
                pafio_manifest(version="1.0"),
                "acme/util",
                "1.0.0",
                [],
            ),
            "request-version-shape": (
                pafio_manifest(),
                "acme/util",
                "1.0",
                [],
            ),
            "dependency-mismatch": (
                pafio_manifest(
                    dependency_source=inline_exact.replace('version = "1.0.0"', 'version = "2.0.0"')
                ),
                "acme/util",
                "1.0.0",
                [exact_dependency],
            ),
            "dependency-version-shape": (
                pafio_manifest(
                    dependency_source=inline_exact.replace('version = "1.0.0"', 'version = "^1.0.0"')
                ),
                "acme/util",
                "1.0.0",
                [exact_dependency],
            ),
            "dependency-not-inline": (
                pafio_manifest(
                    dependency_source="""

[dependencies.alpha]
package = "acme/alpha"
version = "1.0.0"
registry = "https://packages.example.test"
"""
                ),
                "acme/util",
                "1.0.0",
                [exact_dependency],
            ),
            "dependency-extra-field": (
                pafio_manifest(
                    dependency_source=inline_exact.replace(
                        'registry = "https://packages.example.test"',
                        'registry = "https://packages.example.test", path = "../alpha"',
                    )
                ),
                "acme/util",
                "1.0.0",
                [exact_dependency],
            ),
        }

        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            for name, (manifest, package, version, dependencies) in invalid_manifests.items():
                with self.subTest(case=name):
                    archive = root / "inputs" / f"{name}.pafio.src.tar"
                    self._build_source_archive("acme/util", "1.0.0", archive, manifest=manifest)
                    dest_root = root / name / "registry-v2"
                    key_dir = root / name / "keys"
                    with self.assertRaises(RegistryV2Error):
                        self._publish_archive(
                            dest_root,
                            key_dir,
                            archive,
                            package,
                            version,
                            dependencies=dependencies,
                            archive_name=f"{name}.pafio.src.tar",
                        )
                    self.assertFalse(dest_root.exists())
                    self.assertFalse(key_dir.exists())

    def test_publish_accepts_canonical_archive_with_exact_dependencies(self) -> None:
        dependency_source = """

[dependencies]
zeta = { package = "acme/zeta", version = "2.0.0", registry = "https://packages.example.test" }
alpha = { package = "acme/alpha", version = "1.0.0", registry = "https://packages.example.test" }

[dev-dependencies]
fixture = { package = "acme/fixture", version = "3.0.0", registry = "https://packages.example.test" }
"""
        dependencies = [
            {
                "alias": "zeta",
                "package": "acme/zeta",
                "version_req": "2.0.0",
                "registry": "https://packages.example.test",
            },
            {
                "alias": "alpha",
                "package": "acme/alpha",
                "version_req": "1.0.0",
                "registry": "https://packages.example.test",
            },
        ]
        dev_dependencies = [
            {
                "alias": "fixture",
                "package": "acme/fixture",
                "version_req": "3.0.0",
                "registry": "https://packages.example.test",
            }
        ]
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            archive = root / "inputs" / "util.pafio.src.tar"
            self._build_source_archive(
                "acme/util",
                "1.0.0",
                archive,
                manifest=pafio_manifest(dependency_source=dependency_source),
            )
            dest_root = root / "registry-v2"
            key_dir = root / "keys"
            generate_key_directory(key_dir)
            published = self._publish_archive(
                dest_root,
                key_dir,
                archive,
                "acme/util",
                "1.0.0",
                dependencies=dependencies,
                dev_dependencies=dev_dependencies,
            )
            self.assertEqual([entry["alias"] for entry in published["dependencies"]], ["alpha", "zeta"])
            self.assertEqual(published["dev_dependencies"], dev_dependencies)

    def test_publish_rejects_archive_without_pafio_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            dest_root = root / "registry-v2"
            key_dir = root / "keys"
            archive = root / "artifacts" / "util-1.0.0.tar"
            archive.parent.mkdir(parents=True, exist_ok=True)
            archive.write_bytes(
                deterministic_pafio_ustar(
                    [ustar_member("util-1.0.0/README.md", b"missing manifest\n")]
                )
            )
            generate_key_directory(key_dir)

            with self.assertRaises(RegistryV2Error):
                self._publish_archive(dest_root, key_dir, archive, "acme/util", "1.0.0")

    def test_verify_rejects_tampered_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            dest_root = root / "registry-v2"
            key_dir = root / "keys"
            archive = root / "artifacts" / "util-1.0.0.tar"
            self._build_source_archive("acme/util", "1.0.0", archive)
            generate_key_directory(key_dir)
            self._publish_archive(dest_root, key_dir, archive, "acme/util", "1.0.0")

            artifact_path = next((dest_root / "artifacts" / "source" / "sha256").rglob("*.pafio.src.tar"))
            artifact_path.write_bytes(artifact_path.read_bytes() + b"tamper")

            with self.assertRaises(RegistryV2Error):
                verify_registry_root(str(dest_root))

    def test_reader_rejects_non_canonical_object_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            local_reader = RootReader(temp_dir)
            remote_reader = RootReader("https://packages.example.test/pafio/")
            invalid_paths = [
                "../trust/root.json",
                "trust/../root.json",
                "trust/./root.json",
                "trust//root.json",
                "trust\\root.json",
            ]
            for reader in (local_reader, remote_reader):
                for relative_path in invalid_paths:
                    with self.subTest(root=reader.root_value, relative_path=relative_path):
                        with self.assertRaises(RegistryV2Error):
                            reader.location(relative_path)

    def test_publish_initializes_root_and_appends_release(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            archive_root = root / "artifacts"
            archive_root.mkdir(parents=True, exist_ok=True)
            archive_one = archive_root / "util-1.0.0.tar"
            archive_two = archive_root / "util-1.3.0.tar"
            key_dir = root / "keys"
            dest_root = root / "registry-v2"

            self._build_source_archive("acme/util", "1.0.0", archive_one)
            self._build_source_archive("acme/util", "1.3.0", archive_two)
            generate_key_directory(key_dir)

            first = self._publish_archive(
                dest_root,
                key_dir,
                archive_one,
                "acme/util",
                "1.0.0",
                registry_name="unit-registry",
            )
            self.assertTrue(first["created_root"])
            self.assertEqual(first["package"], "acme/util")
            self.assertEqual(first["version"], "1.0.0")
            self.assertEqual(first["publication_id"], "pub-000001")
            first_publication = json.loads((dest_root / "_publications" / "pub-000001" / "publication.json").read_text(encoding="utf-8"))
            self.assertEqual(first_publication["repository_version_id"], "rv-000001")
            self.assertTrue(first_publication["verified"])
            first_current = json.loads((dest_root / "_distributions" / "default" / "current.json").read_text(encoding="utf-8"))
            self.assertEqual(first_current["publication_id"], "pub-000001")

            second = self._publish_archive(
                dest_root,
                key_dir,
                archive_two,
                "acme/util",
                "1.3.0",
                registry_name="unit-registry",
            )
            self.assertFalse(second["created_root"])
            self.assertEqual(second["version"], "1.3.0")
            self.assertEqual(second["sequence"], 2)
            self.assertEqual(second["publication_id"], "pub-000002")
            second_current = json.loads((dest_root / "_distributions" / "default" / "current.json").read_text(encoding="utf-8"))
            self.assertEqual(second_current["publication_id"], "pub-000002")
            self.assertEqual(second_current["previous_publication_id"], "pub-000001")

            verified = verify_registry_root(str(dest_root))
            self.assertTrue(verified["ok"])
            self.assertEqual(verified["releases"], 2)
            self.assertEqual(verified["tree_size"], 2)

            index_lines = (dest_root / "index" / "acme" / "util.jsonl").read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(index_lines), 2)
            versions = [json.loads(line)["version"] for line in index_lines]
            self.assertEqual(versions, ["1.0.0", "1.3.0"])


if __name__ == "__main__":
    unittest.main()
