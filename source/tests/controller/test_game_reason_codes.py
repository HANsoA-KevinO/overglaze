# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
"""Every code the game manager emits is in the one table.

The manager reports codes with parameters and the table in game_reasons.cpp is
the only place that turns them into words. The native tests check every code
the synthetic flow actually reaches; this static check also covers the codes
only an unusual failure reaches (a refusal in a branch no fixture walks), so a
misspelt code cannot ship as "[code]" on the page.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CONTROLLER = ROOT / "source" / "native" / "controller"
TABLE = CONTROLLER / "src" / "game_reasons.cpp"
SOURCES = {name: CONTROLLER / "src" / name for name in ("game_manager.cpp", "game_presentation.cpp", "game_manager_ui.cpp", "games_cli.cpp")}
KEBAB = r"[a-z][a-z0-9]*(?:-[a-z0-9]+)*"
STAGE_LISTS = (("install", "kInstallStages"), ("uninstall", "kUninstallStages"), ("update", "kUpdateStages"), ("repin", "kRepinStages"))


def table_codes():
    text = TABLE.read_text(encoding="utf-8")
    start = text.index("kTable{")
    body = text[start:text.index("};", start)]
    return re.findall(r'\{"([a-z0-9._-]+)","', body)


def strip_comments(text):
    return re.sub(r"//[^\n]*", " ", text)


def source(name):
    return strip_comments(SOURCES[name].read_text(encoding="utf-8"))


def calls(text, head):
    """The top-level arguments of every `head(...)` call; quotes, (), {} and [] are respected."""
    out = []
    for m in re.finditer(r"(?<![\w.])" + re.escape(head) + r"\(", text):
        args, cur, depth, quote, i = [], "", 0, None, m.end()
        while i < len(text):
            ch = text[i]
            if quote:
                cur += ch
                if ch == "\\" and i + 1 < len(text):
                    cur += text[i + 1]
                    i += 1
                elif ch == quote:
                    quote = None
            elif ch in "\"'":
                quote = ch
                cur += ch
            elif ch in "({[":
                depth += 1
                cur += ch
            elif ch in ")}]":
                if depth == 0:
                    args.append(cur.strip())
                    break
                depth -= 1
                cur += ch
            elif ch == "," and depth == 0:
                args.append(cur.strip())
                cur = ""
            else:
                cur += ch
            i += 1
        out.append(args)
    return out


def literals(arg):
    return re.findall(r'"(%s)"' % KEBAB, arg)


def check_name(arg):
    return arg.strip('"') if re.fullmatch(r'"[a-z0-9.-]+"', arg) else None


def emitted():
    """(code, where) for every literal code the sources hand to the table."""
    found = []
    gm, pres, ui = source("game_manager.cpp"), source("game_presentation.cpp"), source("game_manager_ui.cpp")
    for text, where in ((gm, "game_manager.cpp"), (pres, "game_presentation.cpp"), (ui, "game_manager_ui.cpp")):
        for pattern in (r'Refusal\("(%s)"' % KEBAB, r'Reason\{"(%s)"' % KEBAB, r'reasons\.push_back\(\{"(%s)"' % KEBAB,
                        r'changed\(\{"(%s)"' % KEBAB, r'within\("(%s)"' % KEBAB, r'why_not=\{"(%s)"' % KEBAB,
                        r'render\("(%s)"' % KEBAB):
            found += [(c, where) for c in re.findall(pattern, text)]
        # record(s,"name",outcome,value,"reason"): the check name and its reason.
        for args in calls(text, "record"):
            name = check_name(args[1]) if len(args) >= 2 else None
            if name:
                found.append(("check." + name, where))
                if len(args) >= 5:
                    found += [(c, where) for c in literals(args[4])]
        # gate(s,"name",ok,"code","message",...): the check name and the refusal code.
        for args in calls(text, "gate"):
            name = check_name(args[1]) if len(args) >= 2 else None
            if name and len(args) >= 4:
                found.append(("check." + name, where))
                found += [(c, where) for c in literals(args[3])]
    # Preflight checks: add("name",Outcome::x[,value[,"reason"]]).
    for args in calls(gm, "add"):
        if len(args) >= 2 and args[1].startswith("Outcome::") and check_name(args[0]):
            found.append(("check." + check_name(args[0]), "game_manager.cpp preflight"))
            if len(args) >= 4:
                found += [(c, "game_manager.cpp preflight") for c in literals(args[3])]
    # Presentation labels and actions.
    found += [(c, "game_presentation.cpp") for c in re.findall(r'"(label\.%s)"' % KEBAB, pres)]
    found += [(c, "game_presentation.cpp") for c in re.findall(r'"(action\.%s)"' % KEBAB, pres)]
    for args in calls(pres, "action"):
        if args and check_name(args[0]):
            found.append(("action." + check_name(args[0]), "game_presentation.cpp"))
    # Stages, from the stage lists.
    for op, name in STAGE_LISTS:
        body = re.search(name + r"\{([^}]*)\}", gm).group(1)
        found += [("stage.%s.%s" % (op, s), "game_manager.cpp " + name) for s in literals(body)]
    return found


class GameReasonCodes(unittest.TestCase):
    def test_table_is_unique(self):
        codes = table_codes()
        self.assertGreater(len(codes), 150)
        self.assertEqual(sorted(set(c for c in codes if codes.count(c) > 1)), [])

    def test_every_emitted_code_is_in_the_table(self):
        codes = set(table_codes())
        found = emitted()
        self.assertGreater(len(found), 150, "the scan finds the manager's codes")
        missing = sorted({"%s (%s)" % (c, w) for c, w in found if c not in codes})
        self.assertEqual(missing, [])

    def test_steps_name_only_listed_stages(self):
        gm = source("game_manager.cpp")
        listed = set()
        for _, name in STAGE_LISTS:
            listed |= set(literals(re.search(name + r"\{([^}]*)\}", gm).group(1)))
        used = set(re.findall(r'steps\.(?:enter|skip)\("(%s)"' % KEBAB, gm))
        self.assertTrue(used)
        self.assertEqual(sorted(used - listed), [])

    def test_unsupported_route_is_gone(self):
        # The state no preflight verdict could produce is not used any more.
        for name in SOURCES:
            self.assertNotIn('"unsupported-route"', source(name), name)
        self.assertNotIn("allow_observation_only", (CONTROLLER / "include" / "lab_game_manager.hpp").read_text(encoding="utf-8"))

    def test_generated_packages_turn_exception_diagnostics_off(self):
        # write_package is the only writer of a config, and it says false.
        gm = source("game_manager.cpp")
        self.assertIn('{"exception_diagnostics",false}', gm)
        self.assertNotIn('{"exception_diagnostics",true}', gm)


if __name__ == "__main__":
    unittest.main()
