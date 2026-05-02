from __future__ import annotations

import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "publish-spio-tool-release.py"


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


class ToolReleaseTests(unittest.TestCase):
    def test_publish_spio_tool_release_channel(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            registry_root = root / "registry-v2"
            registry_root.mkdir()
            binary = root / "spio"
            binary.write_bytes(b"fake spio binary")
            installer = root / "install-spio.sh"
            installer.write_text("#!/usr/bin/env sh\n", encoding="utf-8")

            proc = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--registry-root",
                    str(registry_root),
                    "--binary",
                    str(binary),
                    "--version",
                    "0.1.0-dev",
                    "--platform",
                    "linux-aarch64",
                    "--install-script",
                    str(installer),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            result = json.loads(proc.stdout)
            self.assertTrue(result["ok"])
            self.assertEqual(result["binary_path"], "tools/spio/releases/0.1.0-dev/linux-aarch64/spio")
            self.assertEqual(
                result["channel_version_path"],
                "tools/spio/channel/latest/linux-aarch64/version",
            )

            latest = json.loads((registry_root / "tools" / "spio" / "latest.json").read_text(encoding="utf-8"))
            entry = latest["platforms"]["linux-aarch64"]
            self.assertEqual(entry["path"], "tools/spio/releases/0.1.0-dev/linux-aarch64/spio")
            self.assertEqual(entry["sha256"], sha256_bytes(b"fake spio binary"))
            self.assertEqual(entry["size_bytes"], len(b"fake spio binary"))
            channel_version = registry_root / "tools" / "spio" / "channel" / "latest" / "linux-aarch64" / "version"
            self.assertEqual(channel_version.read_text(encoding="utf-8"), "0.1.0-dev\n")
            self.assertTrue((registry_root / "tools" / "spio" / "install-spio.sh").exists())

            styio_binary = root / "styio"
            styio_binary.write_bytes(b"fake styio binary")
            styio = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--registry-root",
                    str(registry_root),
                    "--binary",
                    str(styio_binary),
                    "--version",
                    "0.0.1",
                    "--platform",
                    "linux-aarch64",
                    "--tool",
                    "styio",
                    "--binary-name",
                    "styio",
                    "--channel",
                    "stable",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(styio.returncode, 0, styio.stderr)
            styio_result = json.loads(styio.stdout)
            self.assertEqual(styio_result["binary_path"], "tools/styio/releases/0.0.1/linux-aarch64/styio")
            self.assertEqual(
                styio_result["channel_version_path"],
                "tools/styio/channel/stable/linux-aarch64/version",
            )
            self.assertEqual(
                (registry_root / "tools" / "styio" / "channel" / "stable" / "linux-aarch64" / "version").read_text(
                    encoding="utf-8"
                ),
                "0.0.1\n",
            )

            second = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--registry-root",
                    str(registry_root),
                    "--binary",
                    str(binary),
                    "--version",
                    "0.1.0-dev",
                    "--platform",
                    "linux-aarch64",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(second.returncode, 0, second.stderr)

            binary.write_bytes(b"different fake spio binary")
            conflict = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--registry-root",
                    str(registry_root),
                    "--binary",
                    str(binary),
                    "--version",
                    "0.1.0-dev",
                    "--platform",
                    "linux-aarch64",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(conflict.returncode, 0)
            self.assertIn("refusing to overwrite existing artifact", conflict.stderr)


if __name__ == "__main__":
    unittest.main()
