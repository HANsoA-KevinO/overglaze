# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
"""Sign-Release.ps1 with ephemeral keys, and the release public key's consistency.

The release private key is never used here: every signing test creates a
throwaway CNG P-256 key in a temporary folder outside any git work tree. The
written manifest is verified with that key's public blob through .NET and,
when the controller build has it, through the program's own verifier
(lab_self_update_tests.exe --verify).
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "source/powershell/Sign-Release.ps1"
HEADER = ROOT / "source/native/controller/include/lab_self_update.hpp"
PUBLIC_BLOB = ROOT / "assets/release-key/overglaze-release-p256.public.blob"
BUILD = Path(os.environ.get("OVERGLAZE_CONTROLLER_BUILD") or (ROOT / "data/_build_controller"))
VERIFIER = BUILD / "lab_self_update_tests.exe"
POWERSHELL = shutil.which("powershell.exe")
VERSION = "9.9.9-preview.1"
COMMIT = "0123456789abcdef0123456789abcdef01234567"

# [NullString]::Value, not $null: PowerShell passes $null to a string parameter
# as "", and CNG then creates (or finds) a PERSISTED key named "" in the user's
# key store instead of an ephemeral one.
KEYGEN = r"""
$p = [Security.Cryptography.CngKeyCreationParameters]::new()
$p.ExportPolicy = [Security.Cryptography.CngExportPolicies]::AllowPlaintextExport
$k = [Security.Cryptography.CngKey]::Create([Security.Cryptography.CngAlgorithm]::ECDsaP256, [NullString]::Value, $p)
try {
  if (-not $k.IsEphemeral) { throw 'test key is not ephemeral' }
  [IO.File]::WriteAllBytes($env:OG_PRIVATE, $k.Export([Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob))
  [IO.File]::WriteAllBytes($env:OG_PUBLIC, $k.Export([Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob))
} finally { $k.Dispose() }
"""
VERIFY = r"""
$k = [Security.Cryptography.CngKey]::Import([IO.File]::ReadAllBytes($env:OG_PUBLIC), [Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
$e = [Security.Cryptography.ECDsaCng]::new($k); $e.HashAlgorithm = [Security.Cryptography.CngAlgorithm]::Sha256
$s = [Convert]::FromBase64String(([IO.File]::ReadAllText($env:OG_SIG)).Trim())
if ($s.Length -ne 64) { exit 3 }
if ($e.VerifyData([IO.File]::ReadAllBytes($env:OG_MANIFEST), $s)) { exit 0 } else { exit 1 }
"""


def inside_work_tree(path):
    for folder in [path, *path.parents]:
        if (folder / ".git").exists():
            return True
    return False


def powershell(script, **env):
    return subprocess.run([POWERSHELL, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script],
                          env=dict(os.environ, **env), capture_output=True, text=True, errors="replace", timeout=60)


class ReleaseKeyConsistency(unittest.TestCase):
    """The compiled-in key, the repository blob and the published fingerprint agree."""

    def test_header_blob_and_fingerprint_agree(self):
        text = HEADER.read_text(encoding="utf-8")
        block = re.search(r"kReleaseKey\{([^}]*)\}", text).group(1)
        compiled = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", block))
        blob = PUBLIC_BLOB.read_bytes()
        self.assertEqual(len(blob), 72)
        self.assertEqual(blob[:4], b"ECS1")
        self.assertEqual(compiled, blob, "lab_self_update.hpp kReleaseKey differs from the repository's public blob")
        fingerprint = hashlib.sha256(blob).hexdigest()
        self.assertIn(f'kReleaseKeySha256="{fingerprint}"', text)
        self.assertIn(fingerprint, (ROOT / "docs/INSTALLER.md").read_text(encoding="utf-8"), "docs/INSTALLER.md publishes the fingerprint")


@unittest.skipUnless(os.name == "nt" and POWERSHELL, "Windows PowerShell required")
class SignReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="overglaze-sign-release-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve()
        if inside_work_tree(self.base):
            self.skipTest("the temporary folder is inside a git work tree")
        self.keys = self.base / "keys"
        self.keys.mkdir()
        self.private, self.public = self.make_key(self.keys / "test")
        self.out = self.base / "release 中文"
        self.out.mkdir()
        self.setup = self.out / f"Overglaze-Setup-{VERSION}-win64.exe"
        self.zip = self.out / f"Overglaze-{VERSION}-win64.zip"
        self.setup.write_bytes(os.urandom(4096) + b"synthetic setup, never run")
        self.zip.write_bytes(b"PK synthetic zip")
        self.notes = self.base / "notes.txt"
        self.notes.write_bytes("﻿修复：更新清单签名\r\n第二行 \"引号\" 与 \\ 反斜杠\r\n\r\n".encode("utf-8"))

    def make_key(self, stem):
        private, public = stem.with_suffix(".private.blob"), stem.with_suffix(".public.blob")
        result = powershell(KEYGEN, OG_PRIVATE=str(private), OG_PUBLIC=str(public))
        self.assertEqual(result.returncode, 0, result.stderr)
        return private, public

    def sign(self, *extra, key=True, public=None, env=None):
        args = [POWERSHELL, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(SCRIPT),
                "-SetupPath", str(self.setup), "-ZipPath", str(self.zip), "-Version", VERSION, "-Channel", "preview",
                "-SourceCommit", COMMIT, "-NotesFile", str(self.notes), "-Published", "2026-10-20",
                "-PublicKeyPath", str(public or self.public)]
        if key:
            args += ["-KeyPath", str(self.private)]
        environment = dict(os.environ)
        environment.pop("OVERGLAZE_RELEASE_KEY", None)
        environment.update(env or {})
        return subprocess.run(args + list(extra), env=environment, capture_output=True, text=True, errors="replace", timeout=90)

    def outputs(self):
        return self.out / "overglaze-update.json", self.out / "overglaze-update.json.sig"

    def assert_verifies(self, public):
        manifest, signature = self.outputs()
        result = powershell(VERIFY, OG_PUBLIC=str(public), OG_SIG=str(signature), OG_MANIFEST=str(manifest))
        self.assertEqual(result.returncode, 0, ".NET verification failed: " + result.stderr)
        if VERIFIER.is_file():
            native = subprocess.run([str(VERIFIER), "--verify", str(manifest), str(signature), str(public)],
                                    capture_output=True, text=True, errors="replace", timeout=30)
            self.assertEqual(native.returncode, 0, "the program's verifier refused: " + native.stdout + native.stderr)

    def test_signed_manifest_verifies_with_the_key_used(self):
        result = self.sign()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        manifest, signature = self.outputs()
        raw = manifest.read_bytes()
        self.assertFalse(raw.startswith(b"\xef\xbb\xbf"), "UTF-8 without BOM")
        record = json.loads(raw.decode("utf-8"))
        self.assertEqual(list(record), ["schema", "version", "channel", "published", "source_commit", "notes_zh", "setup", "portable"])
        self.assertEqual(record["schema"], "overglaze-update-v1")
        self.assertEqual((record["version"], record["channel"], record["published"], record["source_commit"]), (VERSION, "preview", "2026-10-20", COMMIT))
        self.assertEqual(record["notes_zh"], "修复：更新清单签名\n第二行 \"引号\" 与 \\ 反斜杠")
        for key, path in (("setup", self.setup), ("portable", self.zip)):
            content = path.read_bytes()
            self.assertEqual(record[key], {"name": path.name, "size": len(content), "sha256": hashlib.sha256(content).hexdigest()})
        lines = signature.read_text(encoding="ascii").splitlines()
        self.assertEqual(len(lines), 1)
        self.assertEqual(len(lines[0]), 88)
        self.assert_verifies(self.public)
        self.assertNotIn(self.private.read_bytes().hex(), result.stdout + result.stderr)
        # The release key did not sign this: it must not verify against it.
        if VERIFIER.is_file():
            release = subprocess.run([str(VERIFIER), "--verify", str(manifest), str(signature), str(PUBLIC_BLOB)],
                                     capture_output=True, text=True, timeout=30)
            self.assertNotEqual(release.returncode, 0)

    def test_key_from_the_environment(self):
        result = self.sign(key=False, env={"OVERGLAZE_RELEASE_KEY": str(self.private)})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_verifies(self.public)

    def test_missing_key_is_refused(self):
        result = self.sign(key=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("OVERGLAZE_RELEASE_KEY", result.stdout + result.stderr)
        self.assertFalse(any(p.exists() for p in self.outputs()))

    def test_key_inside_a_git_work_tree_is_refused(self):
        work = self.base / "work tree"
        (work / ".git").mkdir(parents=True)
        inside = work / "secrets" / "release.private.blob"
        inside.parent.mkdir()
        shutil.copyfile(self.private, inside)
        args = [POWERSHELL, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(SCRIPT),
                "-SetupPath", str(self.setup), "-ZipPath", str(self.zip), "-Version", VERSION, "-Channel", "preview",
                "-SourceCommit", COMMIT, "-NotesFile", str(self.notes), "-PublicKeyPath", str(self.public), "-KeyPath", str(inside)]
        result = subprocess.run(args, capture_output=True, text=True, errors="replace", timeout=90)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("git work tree", result.stdout + result.stderr)
        self.assertFalse(any(p.exists() for p in self.outputs()))
        # A worktree or submodule has a .git FILE: refused the same way.
        shutil.rmtree(work / ".git")
        (work / ".git").write_text("gitdir: elsewhere\n", encoding="ascii")
        result = subprocess.run(args, capture_output=True, text=True, errors="replace", timeout=90)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("git work tree", result.stdout + result.stderr)

    def test_wrong_public_key_writes_nothing(self):
        _, other = self.make_key(self.keys / "other")
        result = self.sign(public=other)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not verify", result.stdout + result.stderr)
        self.assertFalse(any(p.exists() for p in self.outputs()))

    def test_file_names_must_match_the_version(self):
        renamed = self.out / "Overglaze-Setup-9.9.8-win64.exe"
        self.setup.rename(renamed)
        self.setup = renamed
        result = self.sign()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SetupPath must be", result.stdout + result.stderr)
        self.assertFalse(any(p.exists() for p in self.outputs()))

    def test_existing_output_is_not_overwritten(self):
        manifest, _ = self.outputs()
        manifest.write_text("keep", encoding="utf-8")
        result = self.sign()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("already exists", result.stdout + result.stderr)
        self.assertEqual(manifest.read_text(encoding="utf-8"), "keep")


if __name__ == "__main__":
    unittest.main()
