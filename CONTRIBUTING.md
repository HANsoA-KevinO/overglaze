# Contributing to Overglaze

Thanks for helping. Please read [POLICY.md](POLICY.md) first. Contributions have to stay inside it.

## What is and isn't accepted

Welcome:

- bug reports with status output, and fixes;
- recipes for new games, with a check report and test notes (see [Game adaptation pull requests](#game-adaptation-pull-requests));
- support for more public Streamline or NGX patterns: new formats, resource layouts, routes;
- documentation and translations.

Not accepted:

- games with anti-cheat, or online or competitive use;
- anything that patches, spoofs, debugs, dumps or works around DRM or anti-tamper, including disguising Overglaze's files to get past a game's checks;
- modified, re-signed or patched model files, or code that loads them;
- uploaded DLLs, EXEs or other binaries, NVIDIA files, or game files and assets taken from games;
- private offsets (RVAs), byte patterns or other reverse-engineered internals of NVIDIA or game binaries;
- code under licences incompatible with MIT, including GPL-licensed code.

Issues that ask for the model or link to it are closed.

## Building

You need:

- 64-bit Windows 11;
- Visual Studio 2022 (MSVC, C++20) with the Windows SDK;
- CMake 3.24 or later and Ninja (the copies that come with Visual Studio work);
- Python 3, for the tests and the compiler output adapter;
- Git, to fetch the dependencies.

The dependencies are fetched at pinned commits and checked by hash: Dear ImGui (docking branch), nlohmann/json, MinHook, and the NVIDIA Streamline public headers (MIT). By default they go to `deps\` in the checkout, which is where the build looks for them (set `OVERGLAZE_DEPS` or `-DLAB_DEPS=<dir>` to use another folder).

The **NVIDIA DLSS (NGX) SDK headers** are proprietary. `Initialize-NgxHeaders.ps1` fetches them from NVIDIA's public DLSS repository only when you pass `-AcceptNvidiaDlssSdkLicense`, i.e. after you have read and accepted NVIDIA's DLSS SDK licence. These headers, like any other NVIDIA SDK file, are never committed to this repository. A CI job that downloads them accepts that licence on the project's behalf, which is why CI documents it.

From the root of the checkout, in PowerShell:

```powershell
# 1. Dependencies, into .\deps (once)
powershell -ExecutionPolicy Bypass -File source\powershell\Initialize-LabDependencies.ps1
powershell -ExecutionPolicy Bypass -File source\powershell\Initialize-StreamlineHeaders.ps1
powershell -ExecutionPolicy Bypass -File source\powershell\Initialize-NgxHeaders.ps1 -AcceptNvidiaDlssSdkLicense

# 2. The x64 developer environment (finds Visual Studio with vswhere)
. .\source\powershell\Enter-Environment.ps1

# 3. Configure and build (a tree without native/research builds the controller track)
cmake -S source -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

`Enter-Environment.ps1 -ToolchainRoot <dir>` uses a self-contained toolchain folder instead of an installed Visual Studio. A build directory outside `<root>\data\_build*` gets an `overglaze-root.json` next to its programs that points them at `build\dev-root\`, so the desktop programs and the tests run from the build directory without writing anywhere else.

`source\powershell\Build-Controller.ps1` does steps 2 and 3 into `data\_build_controller` and then runs every test below.

## Tests

```powershell
# CPU / WARP tests: no GPU and no model needed
ctest --test-dir build -L controller -LE gpu --output-on-failure

# Python tests (the link-closure checks read the linker maps of this build)
$env:OVERGLAZE_CONTROLLER_BUILD = "$PWD\build"
python -B -m unittest discover -s source\tests\controller

# Tests that need a hardware GPU (label gpu)
ctest --test-dir build -L gpu --output-on-failure
```

- The default set (`-LE gpu`) needs no NVIDIA GPU and no model: tests use the CPU, the WARP software rasteriser, or the default adapter Windows provides (the Microsoft Basic Render Driver on a machine without a GPU). Some open a window, so run them in a desktop session.
- Tests that need a hardware GPU carry the `gpu` label and run only when you ask for them; two of them skip themselves when no hardware D3D12 adapter or no D3D12 debug layer is present. `Test-ControllerHost.ps1` drives the real controller host with real NR: it needs an NVIDIA RTX GPU and your own model file in `<program root>\app\models\`.
- GPU tests run one at a time, serialised by the named mutex `Global\Overglaze-ctest` (`Build-Controller.ps1` and `Test-ControllerHost.ps1` take it). Don't bypass it.
- Tests must not depend on local absolute paths. Use temporary directories.

## Rules for code

**Licence headers.** Every new source file starts with SPDX tags. For original code:

```cpp
// SPDX-FileCopyrightText: <year> <your name or handle>
// SPDX-License-Identifier: MIT
```

Contributors add their own `SPDX-FileCopyrightText: <year> <name>` line to files they create or substantially change.

Files derived from third-party code keep the upstream licence identifier and copyright notice; for example, `binding_state.hpp` stays BSD-3-Clause. CI runs `reuse lint`.

**Track boundaries.** The source is split into tracks: core, runtime, provider, viewer and controller (see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#source-tracks)). A file may only include from the tracks its own track is allowed to use. Register every new file in the ownership table. The `track_boundaries` test enforces both.

**Public interfaces only.**
- Overglaze talks to Streamline and NGX only through their exported functions and documented structures.
- Structure versions it doesn't know are refused, not guessed at.
- Modules are identified by their Authenticode signature and pinned SHA-256, never by offsets inside them.

**Checks fail per item.** A check that can fail during a game must:
- have a name, and count its failures;
- fail only the frame or the item it applies to: skip that frame, keep NR on, reset history.

Don't reject a whole call because one unrelated part of it was unexpected. Keep the checks that prevent harm (resource state and lifetime, GPU completion, threads, formats NR has no path for). Never weaken them to make a game "work". NR stops only on a real fault, and writes a fault record when it does.

**Behaviour in the game.**
- No exception handlers, no hiding, no interaction with anti-tamper.
- NR stays off by default.
- No network access, no telemetry.
- Nothing may write outside Overglaze's own files in a game folder.

**No local information.** No absolute paths, user names or machine-specific values in code, tests, comments or logs.

## Commits and pull requests

- Sign off every commit under the [Developer Certificate of Origin](https://developercertificate.org/) with `git commit -s`. This adds a `Signed-off-by:` line. There is no CLA.
- Keep pull requests focused, and say what you tested and on which hardware and driver.
- Never commit binaries, NVIDIA files, game files, captures or large data.

## Game adaptation pull requests

Include:

1. the game-facts JSON in `data/games/<id>.json`, with no local paths;
2. the check report: the output of `overglaze_games preflight "<game EXE>"`, with personal paths removed;
3. test notes:
   - store and game version, GPU, driver;
   - DLSS mode, and frame generation on or off;
   - what the panel showed: route, evidence level, skipped frames and their reasons;
   - whether NR was visibly working.

Recipes that need Denuvo passive coexistence must say so explicitly, and follow [POLICY.md](POLICY.md#denuvo-passive-coexistence-recipes).

See [docs/ADDING-A-GAME.md](docs/ADDING-A-GAME.md) for details.

## Reporting bugs

Include:
- the panel's status lines or `overglazectl <PID> GetStatus` output;
- `nr-fault.json`, if there is one;
- GPU, driver version, the game's store and version, and DLSS and frame-generation settings.

Remove personal paths first. Security problems go through [SECURITY.md](SECURITY.md), not public issues.
