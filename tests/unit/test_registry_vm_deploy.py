from __future__ import annotations

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

from package_registry_v2 import initialize_registry_v2_root, verify_registry_root  # noqa: E402


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
            (registry_root / "config.json").write_text('{"protocol":"spio-static-registry"}', encoding="utf-8")
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
                    self.assertEqual(json.loads(response.read().decode("utf-8"))["protocol"], "spio-static-registry")
                with self.assertRaises(HTTPError) as missing:
                    urlopen(f"http://127.0.0.1:{port}/", timeout=5)
                self.assertEqual(missing.exception.code, 404)
                with self.assertRaises(HTTPError) as mutation:
                    urlopen(Request(f"http://127.0.0.1:{port}/config.json", data=b"{}", method="PUT"), timeout=5)
                self.assertEqual(mutation.exception.code, 405)
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
            control_handler.spio_bin = str(ROOT / "scripts" / "spio")
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
                f"{prefix}/src/PlatformCloud/PackageRegistry/package_registry_v2/publisher.py",
                f"{prefix}/src/PlatformCloud/PackageRegistry/PublicationBuilder/package_registry_v2/publisher.py",
                f"{prefix}/src/PlatformCloud/PackageRegistry/StaticReadPlane/package_registry_v2/validator.py",
            }
            self.assertTrue(required.issubset(names), sorted(required - names))


if __name__ == "__main__":
    unittest.main()
