# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
"""Static dual-track boundary check (source/native/OWNERSHIP.json).

Runs in BOTH build directories (ctest `track_boundaries`) and under
`unittest discover -s source/tests/controller`. It enforces, without compiling:

  R1 files     every file under native/{core,runtime,provider,viewer,controller}/{src,include,tests,host}
               has an OWNERSHIP entry whose track equals its directory, and every entry exists.
  R2 includes  a translation unit may `#include "…"` only files of tracks listed in
               tracks[<its track>].allowed; pending_seams are the only exceptions and each
               must still be a real violation (a closed seam that is still listed fails).
  R3 cmake     each track's CMakeLists.txt may only list sources and link lab_* targets of
               allowed tracks (regex parse; literal paths; ${LAB_NATIVE}/${CMAKE_*_SOURCE_DIR} only).
  R4 ban       targets defined in controller/ and viewer/ never reach controller_ban.libraries
               through their transitive lab_* link closure and never list controller_ban.sources.
  R5 hygiene   parked_targets live in research/CMakeLists.txt with their source in the named
               track dir; noicf_targets carry /OPT:NOICF at their definition; no header basename
               exists in two tracks; no Python test under tests/*/ still uses parents[1].

A tree without native/research (the public export) has an OWNERSHIP.json without the
research track; every rule then covers the five public tracks only.

Limits (by design): CMake is not evaluated — every command inside if()/foreach() counts as
present (conservative); no macro/function calls are allowed in track files; generator
expressions are recognised only for $<TARGET_OBJECTS:x> (counts as a link) and
$<TARGET_FILE:x> (test-time reference, ignored).
"""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "source"
NATIVE = SOURCE / "native"
OWNERSHIP = json.loads((NATIVE / "OWNERSHIP.json").read_text(encoding="utf-8"))
TRACKS = list(OWNERSHIP["tracks"].keys())
KINDS = ("src", "include", "tests", "host")
CODE_SUFFIXES = (".cpp", ".hpp", ".h", ".asm", ".def", ".hlsl")
INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)
CMAKE_CMD_RE = re.compile(r'(?<![\w])([A-Za-z_]\w*)\s*\(((?:[^()]|\([^()]*\))*)\)', re.S)
LAB_TARGET_RE = re.compile(r'^(lab_|overglaze_)')


def allowed(track):
    a = OWNERSHIP["tracks"][track]["allowed"]
    return None if a == ["*"] else set(a)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def disk_files():
    """{track: {kind: {basename: Path}}} for every code file in the six track directories."""
    out = {}
    for track in TRACKS:
        out[track] = {}
        for kind in KINDS:
            d = NATIVE / track / kind
            if d.is_dir():
                out[track][kind] = {p.name: p for p in d.iterdir() if p.is_file() and p.suffix in CODE_SUFFIXES}
    return out


def basename_index(files):
    """basename -> set(tracks) over headers and sources (textual includes of .cpp count too)."""
    index = {}
    for track, kinds in files.items():
        for kind, names in kinds.items():
            for name in names:
                index.setdefault(name, set()).add(track)
    return index


def split_args(body):
    tokens, cur, quote = [], "", None
    for ch in body:
        if quote:
            cur += ch
            if ch == quote:
                quote = None
        elif ch in "\"'":
            quote = ch; cur += ch
        elif ch.isspace():
            if cur: tokens.append(cur); cur = ""
        else:
            cur += ch
    if cur: tokens.append(cur)
    return [t.strip("\"'") for t in tokens if not t.startswith("#")]


def parse_cmake(path):
    """Return list of (command, args) with comments removed."""
    text = "\n".join(line.split("#", 1)[0] if '"' not in line else line for line in path.read_text(encoding="utf-8").splitlines())
    return [(m.group(1).lower(), split_args(m.group(2))) for m in CMAKE_CMD_RE.finditer(text)]


def resolve_source(arg, cmake_dir):
    arg = arg.replace("${LAB_NATIVE}", str(NATIVE)).replace("${CMAKE_CURRENT_SOURCE_DIR}", str(cmake_dir)).replace("${CMAKE_SOURCE_DIR}", str(SOURCE))
    if "${" in arg or arg.startswith("$<"):
        return None
    p = Path(arg)
    return p if p.is_absolute() else (cmake_dir / p)


def track_of_path(p):
    try:
        rel = p.resolve().relative_to(NATIVE.resolve())
    except ValueError:
        return None
    return rel.parts[0] if rel.parts and rel.parts[0] in TRACKS else None


class Graph:
    """Targets, their defining track, sources and lab_* links across all track CMake files."""

    def __init__(self):
        self.defined = {}      # target -> track ('external' for top-level)
        self.sources = {}      # target -> [Path]
        self.links = {}        # target -> [target names]
        self.noicf = set()
        self.tests = {}        # test name -> track
        top = parse_cmake(SOURCE / "CMakeLists.txt")
        for cmd, args in top:
            if cmd in ("add_library", "add_executable") and args:
                self.defined[args[0]] = "external"
        for track in TRACKS:
            f = NATIVE / track / "CMakeLists.txt"
            if not f.exists():
                continue
            for cmd, args in parse_cmake(f):
                if not args:
                    continue
                if cmd in ("add_library", "add_executable"):
                    name = args[0]
                    self.defined[name] = track
                    srcs = [a for a in args[1:] if a not in ("STATIC", "SHARED", "INTERFACE", "OBJECT", "MODULE", "WIN32", "EXCLUDE_FROM_ALL")]
                    self.sources.setdefault(name, []).extend(self._sources(srcs, f.parent, name))
                elif cmd == "target_sources":
                    name = args[0]
                    srcs = [a for a in args[1:] if a not in ("PRIVATE", "PUBLIC", "INTERFACE")]
                    self.sources.setdefault(name, []).extend(self._sources(srcs, f.parent, name))
                elif cmd == "target_link_libraries":
                    name = args[0]
                    self.links.setdefault(name, []).extend(a for a in args[1:] if a not in ("PRIVATE", "PUBLIC", "INTERFACE"))
                elif cmd == "target_link_options" and "/OPT:NOICF" in args:
                    self.noicf.add(args[0])
                elif cmd == "add_test" and "NAME" in args:
                    self.tests[args[args.index("NAME") + 1]] = track

    def _sources(self, args, cmake_dir, target):
        out = []
        for a in args:
            m = re.match(r"\$<TARGET_OBJECTS:(\w+)>", a)
            if m:
                self.links.setdefault(target, []).append(m.group(1)); continue
            p = resolve_source(a, cmake_dir)
            if p is not None and p.suffix in CODE_SUFFIXES:
                out.append(p)
        return out

    def closure(self, target):
        seen, stack = set(), [target]
        while stack:
            t = stack.pop()
            for dep in self.links.get(t, []):
                if LAB_TARGET_RE.match(dep) and dep not in seen:
                    seen.add(dep); stack.append(dep)
        return seen


class TrackBoundaries(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.files = disk_files()
        cls.index = basename_index(cls.files)
        cls.graph = Graph()

    def test_r1_every_file_is_owned(self):
        entries = OWNERSHIP["files"]
        missing, stale = [], []
        for track in TRACKS:
            if track == "research":
                continue
            for kind, names in self.files[track].items():
                for name in names:
                    key = f"{track}/{kind}/{name}"
                    e = entries.get(key)
                    if e is None or e["track"] != track:
                        missing.append(key)
        for key, e in entries.items():
            if not (NATIVE / key).is_file():
                stale.append(key)
        self.assertEqual(missing, [], "files without a matching OWNERSHIP entry")
        self.assertEqual(stale, [], "OWNERSHIP entries whose file no longer exists")

    def test_r2_includes_stay_inside_allowed_tracks(self):
        seams = {(s["file"], i) for s in OWNERSHIP["pending_seams"] for i in ([s["include"]] if isinstance(s["include"], str) else s["include"])}
        seen_seams, violations = set(), []
        for track in TRACKS:
            allow = allowed(track)
            if allow is None:
                continue
            for kind, names in self.files[track].items():
                for name, path in names.items():
                    if path.suffix not in (".cpp", ".hpp", ".h"):
                        continue
                    rel = f"{track}/{kind}/{name}"
                    for inc in INCLUDE_RE.findall(strip_comments(path.read_text(encoding="utf-8", errors="replace"))):
                        base = Path(inc).name
                        targets = self.index.get(base)
                        if not targets:
                            continue  # external header
                        if len(targets) > 1:
                            violations.append(f"{rel}: '{inc}' is ambiguous across tracks {sorted(targets)}"); continue
                        target = next(iter(targets))
                        if target in allow:
                            continue
                        if (rel, base) in seams:
                            seen_seams.add((rel, base)); continue
                        violations.append(f"{rel}: includes '{inc}' from track '{target}' (allowed: {sorted(allow)})")
        self.assertEqual(violations, [], "cross-track includes outside pending_seams")
        self.assertEqual(seams - seen_seams, set(), "pending_seams entries that are no longer real violations: remove them from OWNERSHIP.json")

    def test_r3_cmake_sources_and_links_respect_allowed_tracks(self):
        problems = []
        for target, track in self.graph.defined.items():
            if track == "external":
                continue
            allow = allowed(track)
            if allow is None:
                continue
            for src in self.graph.sources.get(target, []):
                st = track_of_path(src)
                if st is None:
                    problems.append(f"{target} ({track}): source outside native/: {src}")
                elif st not in allow:
                    problems.append(f"{target} ({track}): lists source of track '{st}': {src.name}")
            for dep in self.graph.links.get(target, []):
                if not LAB_TARGET_RE.match(dep):
                    continue
                dt = self.graph.defined.get(dep)
                if dt is None:
                    problems.append(f"{target} ({track}): links undefined target {dep}")
                elif dt != "external" and dt not in allow:
                    problems.append(f"{target} ({track}): links '{dep}' of track '{dt}'")
        self.assertEqual(problems, [])

    def test_r4_controller_link_ban(self):
        ban_libs = set(OWNERSHIP["controller_ban"]["libraries"])
        ban_sources = set(OWNERSHIP["controller_ban"]["sources"])
        problems = []
        for target, track in self.graph.defined.items():
            if track not in ("controller", "viewer"):
                continue
            hit = self.graph.closure(target) & ban_libs
            if hit:
                problems.append(f"{target} ({track}) reaches banned libraries {sorted(hit)}")
            bad = [s.name for s in self.graph.sources.get(target, []) if s.name in ban_sources]
            if bad:
                problems.append(f"{target} ({track}) lists banned sources {bad}")
        self.assertEqual(problems, [])

    def test_r5_parked_targets_are_declared_and_temporary(self):
        problems = []
        for target, home in OWNERSHIP["parked_targets"].items():
            if self.graph.defined.get(target) != "research":
                problems.append(f"{target}: parked target must be defined in research/CMakeLists.txt")
            for src in self.graph.sources.get(target, []):
                if track_of_path(src) != home:
                    problems.append(f"{target}: source {src.name} is not in its home track '{home}'")
        self.assertEqual(problems, [])

    def test_r5_noicf_targets_keep_the_option_at_definition(self):
        missing = [t for t in OWNERSHIP["noicf_targets"] if t in self.graph.defined and t not in self.graph.noicf]
        self.assertEqual(missing, [], "fixture targets that lost /OPT:NOICF (the old DEFER CALL is gone)")

    def test_r5_header_basenames_are_unique_across_tracks(self):
        dupes = {name: sorted(t) for name, t in self.index.items() if len(t) > 1 and name.endswith((".hpp", ".h"))}
        self.assertEqual(dupes, {}, "a header basename in two tracks is picked by /I order silently")

    def test_r5_python_tests_in_subdirectories_use_the_right_root(self):
        bad = [str(p.relative_to(SOURCE)) for p in (SOURCE / "tests").glob("*/test_*.py") if ".parents[1]" in p.read_text(encoding="utf-8", errors="replace") and p.name != Path(__file__).name]
        self.assertEqual(bad, [])

    def test_labels_cover_every_track_directory(self):
        for track in TRACKS:
            f = NATIVE / track / "CMakeLists.txt"
            if track == "research" and not f.exists():
                continue
            text = f.read_text(encoding="utf-8")
            self.assertIn("PROPERTY LABELS", text, f"{track}/CMakeLists.txt must set directory LABELS")


if __name__ == "__main__":
    unittest.main()
