# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
"""The Live ABI header is the controller/research boundary in source form.

The research collectors live outside lab_nr_live_api.hpp. These checks keep it
that way: a single `#include
"lab_frame_capture.hpp"` there would drag the capture types back into the
controller bridge's translation units, which is exactly the coupling the track
split forbids. The compiled counterpart is the ctest `live_abi_layout`, which pins the
structure sizes, and `controller_bridge_exports`, which reads the built
controller bridge's export table.
"""
import re
import unittest
from pathlib import Path

NATIVE = Path(__file__).resolve().parents[2] / "native"
RUNTIME_API = NATIVE / "runtime/include/lab_nr_live_api.hpp"
RESEARCH_API = NATIVE / "research/include/lab_nr_live_research_api.hpp"
SEAM = NATIVE / "runtime/src/nr_live_extension.hpp"
CORE = NATIVE / "runtime/src/nr_live_core.hpp"
EXPORTS = NATIVE / "runtime/src/nr_live.cpp"

# Headers whose types exist only for the research collectors.
RESEARCH_HEADERS = ("lab_frame_capture.hpp", "lab_display_pair.hpp", "lab_binding_probe.hpp",
                    "lab_binding_boundary.hpp", "lab_nr_live_research_api.hpp")
# Collector types that must not appear in the runtime ABI, seam or core.
RESEARCH_TYPES = ("PairStatus", "PairCallMetadata", "FrameCapture", "DisplayPairCapture",
                  "BindingProbe", "BindingBoundaryAudit", "ReplayGuideLayout")


def includes(path):
    return set(re.findall(r'#include\s+"([^"]+)"', path.read_text(encoding="utf-8")))


def body(path):
    """File text without its comments, so prose about capture does not trip the checks."""
    text = re.sub(r"/\*.*?\*/", "", path.read_text(encoding="utf-8"), flags=re.S)
    return "\n".join(re.sub(r"//.*$", "", line) for line in text.splitlines())


class LiveAbiHeader(unittest.TestCase):
    def test_runtime_abi_header_has_no_research_include(self):
        found = includes(RUNTIME_API) & set(RESEARCH_HEADERS)
        self.assertEqual(found, set(), "the runtime Live ABI must not include a collector header")

    def test_runtime_abi_header_has_no_research_type(self):
        text = body(RUNTIME_API)
        present = [t for t in RESEARCH_TYPES if re.search(r"\b" + t + r"\b", text)]
        self.assertEqual(present, [], "collector types belong in lab_nr_live_research_api.hpp")

    def test_seam_and_core_stay_collector_free(self):
        for path in (SEAM, CORE, EXPORTS):
            with self.subTest(file=path.name):
                self.assertEqual(includes(path) & set(RESEARCH_HEADERS), set())
                present = [t for t in RESEARCH_TYPES if re.search(r"\b" + t + r"\b", body(path))]
                self.assertEqual(present, [])

    def test_version_bumped_with_the_split(self):
        text = RUNTIME_API.read_text(encoding="utf-8")
        version = re.search(r"inline constexpr unsigned version=(\d+);", text)
        self.assertIsNotNone(version)
        self.assertGreaterEqual(int(version.group(1)), 20, "ABI20 introduced the split")

    def test_research_api_declares_every_entry_point_named_by_the_runtime(self):
        names = re.findall(r'"(LabNrLive\w+)"', RUNTIME_API.read_text(encoding="utf-8"))
        self.assertEqual(len(names), 15, "the runtime header names the research entry points (15, including the chain capture)")
        if not RESEARCH_API.is_file():
            self.skipTest("native/research is not part of this tree")
        research = RESEARCH_API.read_text(encoding="utf-8")
        # Each research entry point has a signature typedef in the research header.
        suffixes = {n[len("LabNrLive"):] for n in names}
        typedefs = set(re.findall(r"using\s+(\w+)\s*=", research))
        expected = {"SetFrameContext", "PollCapture", "Capture", "ProbeBindings", "PollBindingProbe",
                    "PollPreparationProbe", "BindingBoundary", "PollBindingBoundaries",
                    "DisplayPresent", "DisplayReturned", "DisplayResize", "ChainCapture"}
        self.assertTrue(expected <= typedefs, f"missing research typedefs: {sorted(expected - typedefs)}")
        self.assertIn("DisplayCapture", suffixes)
        self.assertIn("ChainCapture", suffixes)

    def test_the_controller_extension_defines_the_seam_without_collectors(self):
        none = NATIVE / "runtime/src/nr_live_extension_none.cpp"
        text = none.read_text(encoding="utf-8")
        self.assertIn("create_live_extension", text)
        self.assertIn("return nullptr", text)
        self.assertEqual(includes(none) & set(RESEARCH_HEADERS), set())


if __name__ == "__main__":
    unittest.main()
