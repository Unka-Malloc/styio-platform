from __future__ import annotations

import base64
import hashlib
import json
import pathlib
import runpy
import subprocess
import sys
import tarfile
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer
from urllib.error import HTTPError
from urllib.request import Request, urlopen


ROOT = pathlib.Path(__file__).resolve().parents[2]
PACKAGE_REGISTRY = ROOT / "src" / "PlatformCloud" / "PackageRegistry"
if str(PACKAGE_REGISTRY) not in sys.path:
    sys.path.insert(0, str(PACKAGE_REGISTRY))

from PublicationBuilder.package_registry_v2 import initialize_registry_v2_root  # noqa: E402
from StaticReadPlane.package_registry_v2 import verify_registry_root  # noqa: E402


def ustar_octal(value: int, width: int) -> bytes:
    encoded = f"{value:0{width - 1}o}".encode("ascii") + b"\0"
    if len(encoded) != width:
        raise ValueError("ustar fixture numeric field overflow")
    return encoded


def deterministic_pafio_ustar(files: tuple[tuple[str, bytes], ...]) -> bytes:
    archive = bytearray()
    for path, content in files:
        name = path.encode("utf-8")
        if len(name) > 100:
            raise ValueError("VM test fixture path exceeds the ustar name field")
        header = bytearray(512)
        header[0 : len(name)] = name
        header[100:108] = ustar_octal(0o644, 8)
        header[108:116] = ustar_octal(0, 8)
        header[116:124] = ustar_octal(0, 8)
        header[124:136] = ustar_octal(len(content), 12)
        header[136:148] = ustar_octal(0, 12)
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


def pafio_publish_request(*, manifest: bytes | None = None) -> dict[str, object]:
    manifest = manifest or b"""[pafio]
manifest-version = 1

[package]
name = "demo/app"
version = "0.1.0"
edition = "2026"
publish = true

[build]
implicit-std = true

[lib]
path = "src/lib.styio"

[dependencies]
zeta = { package = "demo/zeta", version = "2.0.0", registry = "https://packages.example.test" }
alpha = { package = "demo/alpha", version = "1.0.0", registry = "https://packages.example.test" }

[dev-dependencies]
fixture = { package = "demo/fixture", version = "3.0.0", registry = "https://packages.example.test" }
"""
    archive_bytes = deterministic_pafio_ustar(
        (
            ("app-0.1.0/pafio.toml", manifest),
            ("app-0.1.0/src/lib.styio", b"# app := 1\n"),
        )
    )
    return {
        "package": "demo/app",
        "version": "0.1.0",
        "archive_name": "demo-app-0.1.0.pafio.src.tar",
        "archive_base64": base64.b64encode(archive_bytes).decode("ascii"),
        "archive_sha256": hashlib.sha256(archive_bytes).hexdigest(),
        "archive_size_bytes": len(archive_bytes),
        "publisher_id": "forged-client-publisher",
        "dependencies": [
            {
                "alias": "zeta",
                "package": "demo/zeta",
                "version_req": "2.0.0",
                "registry": "https://packages.example.test",
            },
            {
                "alias": "alpha",
                "package": "demo/alpha",
                "version_req": "1.0.0",
                "registry": "https://packages.example.test",
            },
        ],
        "dev_dependencies": [
            {
                "alias": "fixture",
                "package": "demo/fixture",
                "version_req": "3.0.0",
                "registry": "https://packages.example.test",
            }
        ],
    }


def start_server(handler: type) -> tuple[ThreadingHTTPServer, threading.Thread]:
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


class RegistryVmDeployTests(unittest.TestCase):
    def test_initialize_registry_root_creates_empty_verified_read_plane(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            registry_root = root / "registry-v2"
            key_dir = root / "keys"

            first = initialize_registry_v2_root(str(registry_root), str(key_dir), registry_name="vm-test")
            second = initialize_registry_v2_root(str(registry_root), str(key_dir), registry_name="vm-test")
            verified = verify_registry_root(str(registry_root))

            self.assertTrue(first["created_root"])
            self.assertFalse(second["created_root"])
            self.assertTrue(verified["ok"])
            self.assertEqual(verified["releases"], 0)
            self.assertEqual(json.loads((registry_root / "config.json").read_text(encoding="utf-8"))["registry_name"], "vm-test")

    def test_static_read_server_rejects_mutation_and_directory_listing(self) -> None:
        module = runpy.run_path(str(ROOT / "scripts" / "registry-v2-static-read-server.py"))
        handler = module["RegistryStaticReadHandler"]
        resolve_registry_file = module["resolve_registry_file"]

        with tempfile.TemporaryDirectory() as temp_dir:
            registry_root = pathlib.Path(temp_dir)
            (registry_root / "trust").mkdir(parents=True)
            (registry_root / "config.json").write_text('{"protocol":"pafio-static-registry"}', encoding="utf-8")
            (registry_root / "trust" / "root.json").write_text('{"signed":{"version":1}}', encoding="utf-8")

            self.assertEqual(resolve_registry_file(registry_root, "/trust/root.json"), (registry_root / "trust" / "root.json").resolve())
            for target in ("/", "/trust/../root.json", "/trust//root.json", "/trust%5Croot.json"):
                with self.subTest(target=target):
                    with self.assertRaises((ValueError, IsADirectoryError)):
                        resolve_registry_file(registry_root, target)

            handler.registry_root = registry_root
            server, thread = start_server(handler)
            try:
                port = server.server_port
                with urlopen(f"http://127.0.0.1:{port}/config.json", timeout=5) as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(json.loads(response.read().decode("utf-8"))["protocol"], "pafio-static-registry")
                with self.assertRaises(HTTPError) as missing:
                    urlopen(f"http://127.0.0.1:{port}/", timeout=5)
                self.assertEqual(missing.exception.code, 404)
                missing.exception.close()
                with self.assertRaises(HTTPError) as mutation:
                    urlopen(Request(f"http://127.0.0.1:{port}/config.json", data=b"{}", method="PUT"), timeout=5)
                self.assertEqual(mutation.exception.code, 405)
                mutation.exception.close()
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=5)

    def test_control_plane_rejects_archive_before_creating_registry_state(self) -> None:
        module = runpy.run_path(str(ROOT / "scripts" / "registry-v2-control-plane-server.py"))
        handler = module["RegistryControlPlaneHandler"]

        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            registry_root = root / "registry-v2"
            key_dir = root / "keys"
            handler.registry_root = str(registry_root)
            handler.key_dir = str(key_dir)
            handler.registry_name = "vm-admission"
            server, thread = start_server(handler)
            publish_url = f"http://127.0.0.1:{server.server_port}/api/pafio-registry-control/v1/publish"

            def post(payload: dict[str, object]) -> tuple[int, dict[str, object]]:
                request = Request(
                    publish_url,
                    data=json.dumps(payload).encode("utf-8"),
                    headers={"Content-Type": "application/json"},
                    method="POST",
                )
                try:
                    with urlopen(request, timeout=10) as response:
                        return response.status, json.loads(response.read().decode("utf-8"))
                except HTTPError as error:
                    try:
                        return error.code, json.loads(error.read().decode("utf-8"))
                    finally:
                        error.close()

            bad_checksum = pafio_publish_request()
            checksum_bytes = bytearray(base64.b64decode(str(bad_checksum["archive_base64"])))
            checksum_bytes[0] ^= 1
            bad_checksum["archive_base64"] = base64.b64encode(checksum_bytes).decode("ascii")
            bad_checksum["archive_sha256"] = hashlib.sha256(checksum_bytes).hexdigest()
            bad_checksum["archive_size_bytes"] = len(checksum_bytes)

            publish_false = pafio_publish_request(
                manifest=b"""[pafio]
manifest-version = 1

[package]
name = "demo/app"
version = "0.1.0"
edition = "2026"
publish = false
"""
            )
            invalid_dependency_version = pafio_publish_request()
            dependencies = invalid_dependency_version["dependencies"]
            assert isinstance(dependencies, list)
            dependencies[0]["version_req"] = "^2.0.0"

            invalid_requests = (
                ("checksum", bad_checksum, 422, "PublishError"),
                ("publish-false", publish_false, 422, "PublishError"),
                ("dependency-version", invalid_dependency_version, 400, "UsageError"),
            )
            try:
                for name, payload, expected_status, category in invalid_requests:
                    with self.subTest(case=name):
                        status, response = post(payload)
                        self.assertEqual(status, expected_status, response)
                        self.assertEqual(response["error_payload"]["category"], category)
                        self.assertFalse(registry_root.exists())
                        self.assertFalse(key_dir.exists())
                        self.assertFalse((registry_root / "_staging").exists())
                        self.assertFalse((registry_root / "artifacts").exists())
                        self.assertFalse((registry_root / "index").exists())
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=5)

    def test_control_plane_accepts_only_bounded_pafio_archive_uploads(self) -> None:
        module = runpy.run_path(str(ROOT / "scripts" / "registry-v2-control-plane-server.py"))
        handler = module["RegistryControlPlaneHandler"]
        publish_max_request_bytes = module["PUBLISH_MAX_REQUEST_BYTES"]
        load_json_request = module["load_json_request"]

        class UnreadBody:
            reads = 0

            def read(self, length: int) -> bytes:
                self.reads += 1
                return b""

        oversized_body = UnreadBody()
        fake_handler = type(
            "FakeHandler",
            (),
            {
                "headers": {"Content-Length": str(publish_max_request_bytes + 1)},
                "rfile": oversized_body,
            },
        )()
        with self.assertRaises(ValueError):
            load_json_request(fake_handler, max_request_bytes=publish_max_request_bytes)
        self.assertEqual(oversized_body.reads, 0)

        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            registry_root = root / "registry-v2"
            key_dir = root / "keys"
            initialize_registry_v2_root(str(registry_root), str(key_dir), registry_name="vm-upload")
            handler.registry_root = str(registry_root)
            handler.key_dir = str(key_dir)
            handler.registry_name = "vm-upload"
            server, thread = start_server(handler)
            publish_url = f"http://127.0.0.1:{server.server_port}/api/pafio-registry-control/v1/publish"

            def post(payload: dict[str, object]) -> tuple[int, dict[str, object]]:
                request = Request(
                    publish_url,
                    data=json.dumps(payload).encode("utf-8"),
                    headers={"Content-Type": "application/json"},
                    method="POST",
                )
                try:
                    with urlopen(request, timeout=10) as response:
                        return response.status, json.loads(response.read().decode("utf-8"))
                except HTTPError as error:
                    try:
                        return error.code, json.loads(error.read().decode("utf-8"))
                    finally:
                        error.close()

            try:
                status, response = post(pafio_publish_request())
                self.assertEqual(status, 200, response)
                published = response["payload"]
                self.assertEqual(published["publisher_id"], "control-plane")
                self.assertNotEqual(published["publisher_id"], "forged-client-publisher")
                self.assertEqual([entry["alias"] for entry in published["dependencies"]], ["alpha", "zeta"])
                self.assertTrue(published["artifact_path"].endswith(".pafio.src.tar"))
                for forbidden in ("registry_root", "key_dir", "archive_path", "staging_path"):
                    self.assertNotIn(forbidden, published)
                staging_root = registry_root / "_staging" / "uploads"
                self.assertEqual(list(staging_root.iterdir()), [])

                index_record = json.loads((registry_root / "index" / "demo" / "app.jsonl").read_text(encoding="utf-8"))
                self.assertEqual(index_record["publisher_id"], "control-plane")
                self.assertEqual([entry["alias"] for entry in index_record["dependencies"]], ["alpha", "zeta"])
                self.assertEqual(index_record["dev_dependencies"][0]["alias"], "fixture")

                malformed: list[dict[str, object]] = []
                unexpected_field = pafio_publish_request()
                unexpected_field["manifest_path"] = "/tmp/pafio.toml"
                malformed.append(unexpected_field)
                missing = pafio_publish_request()
                missing.pop("dependencies")
                malformed.append(missing)
                invalid_base64 = pafio_publish_request()
                invalid_base64["archive_base64"] = "YR=="
                malformed.append(invalid_base64)
                wrong_size = pafio_publish_request()
                wrong_size["archive_size_bytes"] = int(wrong_size["archive_size_bytes"]) + 1
                malformed.append(wrong_size)
                wrong_hash = pafio_publish_request()
                wrong_hash["archive_sha256"] = "0" * 64
                malformed.append(wrong_hash)
                unsafe_name = pafio_publish_request()
                unsafe_name["archive_name"] = "../demo-app-0.1.0.pafio.src.tar"
                malformed.append(unsafe_name)

                for payload in malformed:
                    with self.subTest(fields=sorted(payload)):
                        invalid_status, invalid_response = post(payload)
                        self.assertEqual(invalid_status, 400, invalid_response)
                        self.assertEqual(invalid_response["error_payload"]["category"], "UsageError")
                        self.assertEqual(list(staging_root.iterdir()), [])
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=5)

    def test_vm_smoke_passes_against_local_control_and_read_servers(self) -> None:
        control_module = runpy.run_path(str(ROOT / "scripts" / "registry-v2-control-plane-server.py"))
        read_module = runpy.run_path(str(ROOT / "scripts" / "registry-v2-static-read-server.py"))
        smoke_module = runpy.run_path(str(ROOT / "scripts" / "registry-v2-vm-smoke.py"))

        control_handler = control_module["RegistryControlPlaneHandler"]
        read_handler = read_module["RegistryStaticReadHandler"]
        run_smoke = smoke_module["run_smoke"]

        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            registry_root = root / "registry-v2"
            key_dir = root / "keys"
            initialize_registry_v2_root(str(registry_root), str(key_dir), registry_name="vm-smoke")

            control_handler.registry_root = str(registry_root)
            control_handler.key_dir = str(key_dir)
            control_handler.registry_name = "vm-smoke"
            read_handler.registry_root = registry_root

            control_server, control_thread = start_server(control_handler)
            read_server, read_thread = start_server(read_handler)
            try:
                report = run_smoke(
                    f"http://127.0.0.1:{control_server.server_port}",
                    f"http://127.0.0.1:{read_server.server_port}",
                    timeout=5,
                )
            finally:
                control_server.shutdown()
                read_server.shutdown()
                control_server.server_close()
                read_server.server_close()
                control_thread.join(timeout=5)
                read_thread.join(timeout=5)

            self.assertTrue(report["ok"], report)
            self.assertEqual(report["read_config"]["registry_name"], "vm-smoke")

    def test_package_script_creates_vm_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            output_dir = pathlib.Path(temp_dir)
            subprocess.run(["bash", "-n", "scripts/deploy-registry-vm.sh"], cwd=ROOT, check=True)
            subprocess.run(["bash", "-n", "scripts/package-registry-server.sh"], cwd=ROOT, check=True)
            subprocess.run(
                [
                    "bash",
                    "scripts/package-registry-server.sh",
                    "--version",
                    "unit-test",
                    "--output-dir",
                    str(output_dir),
                ],
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=True,
            )

            archive_path = output_dir / "styio-platform-registry-server-unit-test.tar.gz"
            self.assertTrue(archive_path.exists())
            with tarfile.open(archive_path, "r:gz") as archive:
                names = set(archive.getnames())

            prefix = "styio-platform-registry-server-unit-test"
            required = {
                f"{prefix}/install.sh",
                f"{prefix}/MANIFEST.json",
                f"{prefix}/README.md",
                f"{prefix}/scripts/registry-v2-control-plane-server.py",
                f"{prefix}/scripts/registry-v2-static-read-server.py",
                f"{prefix}/scripts/registry-v2-vm-smoke.py",
                f"{prefix}/src/PlatformCloud/PackageRegistry/package_registry_v2/__init__.py",
                f"{prefix}/src/PlatformCloud/PackageRegistry/PublicationBuilder/package_registry_v2/publisher.py",
                f"{prefix}/src/PlatformCloud/PackageRegistry/StaticReadPlane/package_registry_v2/validator.py",
            }
            self.assertTrue(required.issubset(names), sorted(required - names))


if __name__ == "__main__":
    unittest.main()
