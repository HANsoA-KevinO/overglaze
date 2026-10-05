# Changelog

All notable changes to Overglaze are listed here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/).

## [0.1.0] - 2026-10-05


Initial public release.

### Added

**In-game panel** (Insert, or F7, F8 or F9):
- NR on/off, off at every game start;
- Tone and Structure (0–2), Style (0, 1, 2), Skin with AutoMask;
- input exposure, manual or automatically metered;
- compare split, and compute-only mode;
- read-back of the parameters the model actually used, and evidence levels L1–L3;
- skip and history counters, with named reasons.

**Routes:**
- Streamline Ray Reconstruction and Super Resolution, through Streamline's public interface;
- NGX direct Ray Reconstruction and Super Resolution, including display-resolution motion vectors and letterboxed images.

**Insertion:**
- after the upscaler, on the same command list, with the result composited back in place;
- restores the game's bindings;
- each check is named and counted, and a failing frame is skipped while NR stays on.

**Colour handoff:** compress before NR, and transfer NR's change back as a per-channel ratio.

**Loading:**
- proxy DLL;
- proxy DLL that waits for the first Insert;
- late load through the Steam launch-option launcher (`overglaze_launch.exe`), `overglaze_games attach` or `overglaze_games watch`.

**Desktop program** (`overglaze_viewer.exe`), Games page:
- add and check games, generate adapter packages;
- install, update, re-adapt after game updates, uninstall;
- health check, and storage overview.

**Command-line game manager** (`overglaze_games.exe`), with the same functions and a dry-run install plan.

**Local control pipe** (protocol 1.0) and a command-line client.

**Model checks:** SHA-256 against known versions, on the desktop, at install, and in the game before and after loading.

**Recipes:** six confirmed games and a set of experimental ones; see [SUPPORTED_GAMES.md](SUPPORTED_GAMES.md).

### Known limitations

- Only one model version is known: 310.8.0.0.
- NR together with DLSS Frame Generation has not been checked systematically.
- Image quality varies by game, and is not equivalent to an official NR integration.
- Tested only on Windows 11 with an RTX 5090.
- The desktop program and the panel are available in Chinese only.
