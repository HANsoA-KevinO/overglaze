# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
"""Out-of-game path scan.

Scope: every code file under native/controller and native/viewer (src, include,
tests), those two CMakeLists.txt, and the Python tests in this directory -- the
programs that run outside a game AND their tests (a CI on another machine fails
on a test that hard-codes a developer machine just as surely as on product code).

  M  machine paths   no drive-letter literal that points into a checkout or
                     program root (data\\_build*, source\\native, app\\models ...),
                     a games library folder, a toolchain
                     folder or a user profile. Programs resolve their root at run
                     time (lab_root_locator); tests use temporary directories.
  L  layout guessing no production source (src/, include/) derives a root by
                     walking parent_path().parent_path() of a program path, except
                     where the rule itself is defined (listed below).

Generic drive-letter strings used as test DATA (a command line to parse, two
spellings to compare, C:\\g\\game.exe) are not machine paths and are not flagged.

EXCEPTIONS lists each remaining match with why and the condition that closes
it. An exception that no longer matches anything fails as well, so the list only
shrinks for real. This file itself is excluded: it has to spell the patterns it
looks for.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "source"
NATIVE = SOURCE / "native"
SELF = Path(__file__).resolve()
CODE_SUFFIXES = (".cpp", ".hpp", ".h", ".asm", ".def", ".hlsl")

SEP = r"(?:\\\\|\\|/)"
DRIVE = r"(?<![A-Za-z0-9_])[A-Za-z]:" + SEP
ANY = r"[^\"'\n]*?"
MACHINE = [
    ("checkout-tree", re.compile(DRIVE + ANY + r"(?:data" + SEP + r"_build|source" + SEP + r"native|app" + SEP + r"(?:models|adapters|plugin|tools|research)\b)", re.I)),
    # A games library on a developer's disk. "steamapps" alone is the generic Steam
    # layout that tests parse, so it is not a machine path.
    ("game-library", re.compile(DRIVE + ANY + r"(?:Steam" + r"Library|XboxGames)", re.I)),
    ("toolchain", re.compile(DRIVE + ANY + r"(?:_tool" + r"chains|VSBuildTools|Microsoft Visual Studio|Python3\d)", re.I)),
    ("user-profile", re.compile(DRIVE + r"Users" + SEP, re.I)),
]
LAYOUT = ("layout", re.compile(r"parent_path\(\)\s*\.\s*parent_path\(\)"))

# (file under source/, rule, substring of the line, why, closing condition)
EXCEPTIONS = [
    # ---- in-game code: layout arithmetic against a root the caller names.
    ("native/controller/src/controller_host.cpp", "layout", "need(exe.parent_path().parent_path()==fixture_data_root(),",
     "游戏内宿主的夹具准入：夹具 EXE 必须在夹具数据根（其运行目录 OVERGLAZE_LIVE_DATA_PATH 的上一级，经本地固定盘规则核验）下的运行目录里。比较的是调用方给出的根，不是推导程序根。",
     "永久：这就是夹具准入规则本身。"),
    ("native/viewer/src/ui_preferences.cpp", "layout", "const auto root=data_root.empty()?file.parent_path().parent_path():data_root;",
     "偏好文件的数据根由调用方传入（安装记录的数据根、查看器解析出的根）；没传时取文件的上两级，仍要满足同一形状。",
     "永久：调用方都传根之后可以去掉默认分支。"),
    ("native/viewer/src/ui_preferences.cpp", "layout", "file.parent_path().parent_path()==root",
     "偏好文件必须是数据根的直接子目录里的文件（游戏内约束，根由调用方给出）。", "永久（规则定义处）。"),
    # ---- capture layout checks against a root the caller gives
    ("native/controller/src/console.cpp", "layout", "lab::check(file.parent_path().parent_path()==root && file.parent_path().filename()",
     "开发控制台的采集自测按 <数据根>\\manual-capture-*\\ 检查清理范围；根来自 lab_root_locator。", "永久（检查形状本身）。"),
    ("native/viewer/src/pair_preview.cpp", "layout", "path.parent_path().parent_path().parent_path()==root",
     "按 <数据根>\\<运行>\\<配对>\\manifest.json 检查清单位置；数据根由调用方传入。", "永久（检查形状本身）。"),
    ("native/viewer/src/export_sdr.cpp", "layout", "const auto data_root=std::filesystem::canonical(raw.path()).parent_path().parent_path().parent_path();",
     "命令行导出工具把导出放进采集本身所在的数据根（<数据根>\\<运行>\\<配对>\\manifest.json）；导出根检查再要求它名为 data 且在本地固定盘上。",
     "永久（采集布局就是这个形状）。"),
    # ---- the locator itself: the one place that knows the layout
    ("native/controller/src/root_locator.cpp", "layout", "root=dir.parent_path().parent_path();out.source=Source::build_layout;",
     "根解析本身：<根>\\data\\_build*\\ 布局就是在这里认的，其余游戏外程序都调用它。", "永久（这是规则的定义处）。"),
]


def scope():
    files = []
    for track in ("controller", "viewer"):
        for kind in ("src", "include", "tests"):
            d = NATIVE / track / kind
            if d.is_dir():
                files += sorted(p for p in d.iterdir() if p.is_file() and p.suffix in CODE_SUFFIXES)
        files.append(NATIVE / track / "CMakeLists.txt")
    files += sorted(p for p in (SOURCE / "tests" / "controller").glob("*.py") if p.resolve() != SELF)
    return files


def production(path):
    rel = path.relative_to(NATIVE).parts if NATIVE in path.parents else ()
    return len(rel) >= 2 and rel[1] in ("src", "include")


def scan():
    """[(file under source/, rule, line number, line)]: rule "machine" (any MACHINE
    pattern; the kinds are appended to the line text) or "layout", once per line."""
    hits = []
    for path in scope():
        text = path.read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), 1):
            kinds = [kind for kind, pattern in MACHINE if pattern.search(line)]
            if kinds:
                hits.append((path.relative_to(SOURCE).as_posix(), "machine", number, line.strip() + "    <" + ",".join(kinds) + ">"))
            if production(path) and LAYOUT[1].search(line):
                hits.append((path.relative_to(SOURCE).as_posix(), "layout", number, line.strip()))
    return hits


def excepted(hit):
    file, rule, _, line = hit
    return [e for e in EXCEPTIONS if e[0] == file and e[1] == rule and e[2] in line]


class OutOfGamePaths(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.hits = scan()

    def test_scope_is_not_empty(self):
        files = scope()
        self.assertGreater(len(files), 40)
        for name in ("native/controller/src/viewer.cpp", "native/controller/src/games_cli.cpp", "native/controller/src/launcher.cpp",
                     "native/controller/src/game_manager.cpp", "native/controller/tests/game_manager_tests.cpp", "native/viewer/src/preview_export.cpp"):
            self.assertIn(SOURCE / name, files, name)

    def test_no_machine_path_or_layout_guess_outside_the_exception_list(self):
        loose = [f"{f}:{n}: [{r}] {l[:160]}" for f, r, n, l in self.hits if not excepted((f, r, n, l))]
        self.assertEqual(loose, [], "machine-specific path or layout guess in out-of-game code; fix it, or list it with a closing condition")

    def test_every_exception_still_matches_and_is_complete(self):
        stale = [e[:3] for e in EXCEPTIONS if not any(excepted(h) and e in excepted(h) for h in self.hits)]
        self.assertEqual(stale, [], "an exception that matches nothing is closed: remove it")
        for e in EXCEPTIONS:
            self.assertEqual(len(e), 5)
            self.assertTrue((SOURCE / e[0]).is_file(), e[0])
            self.assertIn(e[1], ("machine", "layout"))
            self.assertTrue(e[3] and e[4], f"{e[0]}: reason and closing condition are both required")

    def test_the_rules_bite(self):
        # The patterns themselves, on lines they must and must not flag.
        machine = lambda s: [r for r, p in MACHINE if p.search(s)]
        self.assertIn("checkout-tree", machine(r'L"D:\\work\\overglaze\\data\\_build_controller"'))
        self.assertIn("checkout-tree", machine(r'"E:/x/Lab/app/models/nvngx_dlssnr.dll"'))
        self.assertIn("game-library", machine(r'L"I:\\Steam' + r'Library\\steamapps"'))
        self.assertIn("game-library", machine(r'L"E:\\XboxGames\\Game\\Content"'))
        self.assertIn("toolchain", machine(r'"D:/tools/_tool' + 'chains/Python312"'))
        self.assertIn("user-profile", machine(r'L"C:\\' + r'Users\\someone\\AppData"'))
        for clean in (r'L"C:\\g\\game.exe -x"', r'LR"("C:\Lab Root\app\overglaze_launch.exe")"', r'L"D:\\Games\\dxgi.dll"',
                      r'"<root>\\app\\overglaze_launch.exe"', "std::filesystem::path p", r'"http://example"', r'L"steamapps"'):
            self.assertEqual(machine(clean), [], clean)
        self.assertTrue(LAYOUT[1].search("const auto root=self.parent_path().parent_path();"))
        self.assertFalse(LAYOUT[1].search("library_root().parent_path()"))


if __name__ == "__main__":
    unittest.main()
