# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
"""Packaging regression tests with tiny fake binaries, never a model or game.

Each test gets a separate temporary Git fixture and two empty output roots.
Fixture commits are private test data, not commits to the real checkout.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "source/powershell/Package-Overglaze.ps1"
POWERSHELL = shutil.which("powershell.exe")
GIT = shutil.which("git")
VERSION = "0.2.0-preview.1"
NAME = f"Overglaze-{VERSION}-win64"
BINARIES = (
    "overglaze_viewer.exe", "overglazectl.exe", "overglaze_games.exe", "overglaze_launch.exe",
    "controller/dxgi.dll", "controller/overglaze_nvngx.dll", "controller/overglaze_controller.dll",
    "overglaze_export_sdr.exe", "overglaze_capture_stats.exe",
    "overglaze_check_view_reconstruction.exe", "lab_installation_tests.exe",
)
DOCUMENTS = (
    "README.md", "README.zh-CN.md", "LICENSE", "THIRD_PARTY_NOTICES.md", "POLICY.md",
    "SUPPORTED_GAMES.md", "CHANGELOG.md", "SECURITY.md", "CONTRIBUTING.md",
    "LICENSES/MIT.txt", "LICENSES/BSD-2-Clause.txt", "LICENSES/BSD-3-Clause.txt", "LICENSES/Apache-2.0.txt",
    "docs/MODEL.md", "docs/ADDING-A-GAME.md", "docs/TROUBLESHOOTING.md",
    "docs/ARCHITECTURE.md", "docs/CONTROL-PROTOCOL.md", "docs/PORTABLE.md",
)


@unittest.skipUnless(os.name == "nt" and POWERSHELL and GIT, "Windows PowerShell and Git required")
class PortablePackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="overglaze-package-test-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.repo = self.base / "repo with spaces"
        self.build = self.base / "build with spaces"
        self.output = self.base / "output"
        self.repo.mkdir()
        self.build.mkdir()
        for relative in DOCUMENTS:
            target = self.repo / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(f"Public fixture: {relative}\n", encoding="utf-8")
        for relative in BINARIES:
            target = self.build / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"MZ\0fixture\0" + relative.encode("ascii"))
        self.git("init", "--quiet")
        self.git("add", ".")
        self.git("-c", "user.name=Overglaze test", "-c", "user.email=test@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "Fixture")

    def git(self, *args):
        return subprocess.run([GIT, "-C", str(self.repo), *args], check=True, capture_output=True)

    def package(self, output=None, build=None, version=VERSION, succeeds=True):
        result = subprocess.run([
            POWERSHELL, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(SCRIPT),
            "-RepositoryRoot", str(self.repo), "-BuildDirectory", str(build or self.build),
            "-OutputDirectory", str(output or self.output), "-Version", version,
        ], capture_output=True, text=True, errors="replace", timeout=45)
        if succeeds:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def manifest(self, output=None):
        return json.loads(((output or self.output) / NAME / "app/release-manifest.json").read_text("utf-8"))

    def test_allowlist_manifest_and_archive_are_reproducible(self):
        # These unlisted files must not enter the archive, even under a public
        # document directory. A root override would leak a local path.
        (self.build / "nvngx_dlssnr.dll").write_bytes(b"DO NOT DISTRIBUTE")
        (self.build / "overglaze-root.json").write_text(json.dumps({"local": str(self.base)}))
        (self.repo / "docs/private-note.txt").write_text("PRIVATE")
        self.package()
        other = self.base / "output two"
        self.package(output=other)
        one = (self.output / f"{NAME}.zip").read_bytes()
        self.assertEqual(one, (other / f"{NAME}.zip").read_bytes())
        manifest = self.manifest()
        self.assertTrue(manifest["source_dirty"])
        self.assertFalse(manifest["model_included"])
        self.assertEqual(manifest["version"], VERSION)
        self.assertNotIn(str(self.base), json.dumps(manifest))
        expected = {"app/" + name for name in BINARIES[:4]}
        expected |= {"app/plugin/" + Path(name).name for name in BINARIES[4:7]}
        expected |= {"app/tools/" + Path(name).name for name in BINARIES[7:10]}
        expected |= {"app/tools/overglaze_install_check.exe", "app/models/README.txt", "Open-Overglaze.cmd"}
        expected |= set(DOCUMENTS)
        self.assertEqual({item["path"] for item in manifest["files"]}, expected)
        for item in manifest["files"]:
            content = (self.output / NAME / item["path"]).read_bytes()
            self.assertEqual(item["size"], len(content))
            self.assertEqual(item["sha256"], hashlib.sha256(content).hexdigest())
        with zipfile.ZipFile(self.output / f"{NAME}.zip") as archive:
            self.assertEqual(set(archive.namelist()), {NAME + "/" + path for path in expected | {"app/release-manifest.json"}})
            self.assertTrue(all(entry.date_time == (2000, 1, 1, 0, 0, 0) for entry in archive.infolist()))
        self.assertEqual((self.output / f"{NAME}.zip.sha256").read_text().split()[0], hashlib.sha256(one).hexdigest())

    def test_clean_checkout_is_reported_clean(self):
        self.package()
        self.assertFalse(self.manifest()["source_dirty"])
        self.assertEqual(self.manifest()["source_commit"], self.git("rev-parse", "HEAD").stdout.decode().strip())

    def test_existing_output_is_never_overwritten(self):
        self.package()
        archive = self.output / f"{NAME}.zip"
        before = archive.read_bytes()
        result = self.package(succeeds=False)
        self.assertIn("Output already exists", result.stderr)
        self.assertEqual(archive.read_bytes(), before)

    def test_missing_input_fails_before_creating_stage(self):
        (self.build / "overglaze_viewer.exe").unlink()
        self.package(succeeds=False)
        self.assertFalse(self.output.exists())

    def test_paths_and_version_cannot_escape_output(self):
        self.package(version="../escape", succeeds=False)
        self.assertFalse(self.output.exists())
        self.package(output=self.build / "package", succeeds=False)
        self.assertFalse((self.build / "package").exists())
        self.package(output=self.repo, succeeds=False)
        self.assertFalse((self.repo / NAME).exists())

    def test_junction_build_is_rejected(self):
        junction = self.base / "linked build"
        # Paths come from the fixture, not from shell interpolation.
        environment = dict(os.environ, OVERGLAZE_TEST_LINK=str(junction), OVERGLAZE_TEST_TARGET=str(self.build))
        result = subprocess.run([POWERSHELL, "-NoProfile", "-NonInteractive", "-Command",
            "New-Item -ItemType Junction -Path $env:OVERGLAZE_TEST_LINK -Target $env:OVERGLAZE_TEST_TARGET | Out-Null"],
            env=environment, capture_output=True, timeout=15)
        if result.returncode:
            self.skipTest("Junction creation unavailable on this filesystem")
        result = self.package(build=junction, succeeds=False)
        self.assertIn("Reparse point refused", result.stderr)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
