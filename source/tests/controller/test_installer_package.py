# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
"""Installer input validation, without compiling, installing or running apps.

Tiny x64 PE headers allow failure cases to reach the intended manifest/path
checks. They cannot pass the embedded-version or Microsoft signature checks.
The real installer lifecycle is covered by Test-Installer.ps1 separately.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "source/powershell/Build-Installer.ps1"
POWERSHELL = shutil.which("powershell.exe")
RUNTIME = (
    "concrt140.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
    "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll", "vccorlib140.dll",
    "vcruntime140.dll", "vcruntime140_1.dll", "vcruntime140_threads.dll",
)
BINARIES = (
    "app/overglaze_viewer.exe", "app/overglazectl.exe", "app/overglaze_games.exe",
    "app/overglaze_launch.exe", "app/plugin/dxgi.dll", "app/plugin/overglaze_nvngx.dll",
    "app/plugin/overglaze_controller.dll", "app/tools/overglaze_export_sdr.exe",
    "app/tools/overglaze_capture_stats.exe", "app/tools/overglaze_check_view_reconstruction.exe",
    "app/tools/overglaze_install_check.exe",
)


@unittest.skipUnless(os.name == "nt" and POWERSHELL, "Windows PowerShell required")
class InstallerPackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="overglaze-installer-validate-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.payload = self.base / "payload 中文 with spaces"
        self.output = self.base / "output"
        pe = bytearray(128)
        struct.pack_into("<H", pe, 0, 0x5A4D)
        struct.pack_into("<I", pe, 0x3C, 64)
        struct.pack_into("<IH", pe, 64, 0x4550, 0x8664)
        for relative in BINARIES + tuple("app/" + name for name in RUNTIME):
            target = self.payload / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(pe)
        for relative in ("LICENSE", "app/models/README.txt", "Open-Overglaze.cmd"):
            target = self.payload / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("Public fixture\n", encoding="utf-8")
        self.record = {
            "schema": "overglaze-release-v2", "platform": "windows-x64",
            "version": "0.2.0-preview.3", "channel": "preview", "source_commit": "a" * 40,
            "source_dirty": False, "model_included": False,
            "files": [],
        }
        self.refresh_manifest()

    def refresh_manifest(self):
        self.record["files"] = []
        for target in sorted(self.payload.rglob("*")):
            if target.is_file() and target.name != "release-manifest.json":
                content = target.read_bytes()
                self.record["files"].append({
                    "path": target.relative_to(self.payload).as_posix(),
                    "size": len(content), "sha256": hashlib.sha256(content).hexdigest(),
                })
        self.write_manifest()

    def write_manifest(self):
        (self.payload / "app/release-manifest.json").write_text(json.dumps(self.record), encoding="utf-8")

    def refused(self, expected, *, payload=None, output=None):
        result = subprocess.run([
            POWERSHELL, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
            "-File", str(SCRIPT), "-PayloadDirectory", str(payload or self.payload),
            "-OutputDirectory", str(output or self.output), "-ValidateOnly",
        ], capture_output=True, text=True, errors="replace", timeout=30)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(expected, result.stdout + result.stderr)
        self.assertFalse(self.output.exists(), "Validation must not create installer output")
        return result

    def test_declared_model_inclusion_is_refused(self):
        self.record["model_included"] = True
        self.write_manifest()
        self.refused("Invalid model-free Windows release manifest")

    def test_model_file_in_manifest_is_refused(self):
        (self.payload / "app/models/nvngx_dlssnr.dll").write_bytes(b"MODEL MUST NOT SHIP")
        self.refresh_manifest()
        self.refused("Invalid or unlisted payload entry")

    def test_unlisted_executable_is_refused(self):
        (self.payload / "app/other.exe").write_bytes(b"MZ")
        self.refused("Unlisted file in portable payload")

    def test_tampered_payload_is_refused(self):
        target = self.payload / "app/overglaze_games.exe"
        target.write_bytes(target.read_bytes() + b"TAMPERED")
        self.refused("Payload size/hash mismatch")

    def test_duplicate_manifest_entry_is_refused(self):
        self.record["files"].append(dict(self.record["files"][0]))
        self.write_manifest()
        self.refused("Invalid or unlisted payload entry")

    def test_runtime_is_required_for_offline_bootstrap(self):
        (self.payload / "app/vcruntime140_threads.dll").unlink()
        self.refresh_manifest()
        self.refused("Required installer payload is missing")

    def test_manifest_cannot_escape_its_root(self):
        self.record["files"][0]["path"] = "../outside.exe"
        self.write_manifest()
        self.refused("Invalid or unlisted payload entry")

    def test_wrong_architecture_is_refused(self):
        target = self.payload / "app/overglaze_games.exe"
        content = bytearray(target.read_bytes())
        struct.pack_into("<H", content, 68, 0x14C)
        target.write_bytes(content)
        self.refresh_manifest()
        self.refused("Not an x64 Windows PE")

    def test_manifest_version_cannot_escape_output(self):
        self.record["version"] = "../escape"
        self.write_manifest()
        self.refused("Invalid model-free Windows release manifest")

    def test_valid_header_does_not_replace_embedded_version(self):
        self.refused("Viewer embedded product version does not match")

    def test_output_cannot_overlap_input(self):
        self.refused("Installer output and payload must not contain each other",
                     output=self.payload / "installer")
        self.assertFalse((self.payload / "installer").exists())

    def test_junction_is_refused_before_traversal(self):
        junction = self.base / "linked payload"
        environment = dict(os.environ, OVERGLAZE_TEST_LINK=str(junction),
                           OVERGLAZE_TEST_TARGET=str(self.payload))
        result = subprocess.run([
            POWERSHELL, "-NoProfile", "-NonInteractive", "-Command",
            "New-Item -ItemType Junction -Path $env:OVERGLAZE_TEST_LINK -Target $env:OVERGLAZE_TEST_TARGET | Out-Null",
        ], env=environment, capture_output=True, timeout=15)
        if result.returncode:
            self.skipTest("Junction creation unavailable on this filesystem")
        self.refused("Reparse point refused", payload=junction)


if __name__ == "__main__":
    unittest.main()
