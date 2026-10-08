# Changelog

All notable changes to Overglaze are listed here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/).

## [0.2.0-preview.4] - unreleased

### Changed

- Denuvo Anti-Tamper and anti-cheat are now risks the user is told about, not refusals. The check still names what it found: Denuvo sections in the EXE, and anti-cheat files by the product their names point to (Easy Anti-Cheat, BattlEye, EQU8, Vanguard, Anti-Cheat Expert) or by file name. Games with either get the ordinary states and actions, with a neutral tag on the Games page. Other loaders in the game folder and an unidentifiable store package are still refused.
- One risk acknowledgement replaces the offline / no-anti-cheat declaration. In the desktop program, the install, update and re-adapt confirmation lists what the check found and states that online or anti-cheat games may not start, may kick you or penalise or ban your account, and that Overglaze never hides from or bypasses any protection; choosing *Install* or *Update* is the acknowledgement. There is no extra checkbox. On the command line it is `--accept-risk`; `--offline --no-anticheat` and `--denuvo-passive-coexistence` remain accepted as aliases. The transaction records the acknowledgement and the risks found.
- Games whose EXE carries Denuvo are set up from the Games page like any other game. *Generate adapter package* gives them late loading (nothing in the game folder's root; Resident Evil Requiem crashes with any `dxgi.dll` there) and shows the planned loading method beforehand. Every other game keeps the proxy DLL.
- After installing a late-loading game, the Games page shows the exact Steam launch option, built from the program's own folder, with a copy button and where to set it in Steam; it stays in the game's details. Games not started from Steam get the `overglaze_games watch` command instead. Uninstalling such a game reminds you to remove the launch option.

### Installation configuration

- Packages generated for the controller now write configuration version 4: the user's risk acknowledgement and the facts the check found (`risk`: `acknowledged`, `anti_tamper`, `anticheat`) replace version 3's `offline_single_player` / `no_anticheat`, so a game with anti-cheat is never recorded as having none. The in-game host and the install checker accept versions 3 and 4, so plugins installed by earlier versions keep working unchanged. Research-track packages still write version 3 and are refused for games with anti-cheat files.
- After updating the application, installed games show *update available*; *Update plugin* rewrites their package with version 4. Until then they keep working as installed.

## [0.2.0-preview.3] - 2026-10-07

### Changed

- Shortened desktop, first-run, in-game panel and installer copy. Removed slogans and repeated explanations; retained installation consent, actual status and diagnostic details.
- Renamed Settings & About to Settings. Model configuration and empty states use direct labels.

## [0.2.0-preview.2] - 2026-10-07

### Changed

- Reworked the desktop Game Library with name/path search, installed/needs-attention filters, selected-game actions, and expandable checks and installation details.
- Reorganised the in-game panel around observed NR status, Style, Tone, Structure, Skin/AutoMask, input exposure and split comparison. Model read-backs, skip reasons, compute-only mode and panel settings remain available under Advanced & Diagnostics.
- Applied a shared charcoal-and-pale-green visual style to the desktop and in-game interfaces. The capture browser continues to operate on saved data.
- Integrated provider updates through `61b5279`: ordinary Presents no longer end a frozen in-flight call or expire Lab-owned guide copies, and non-nested outer-admission refusals skip rather than immediately stopping NR. These code changes are not new real-game acceptance results; outstanding review limits are recorded in `docs/UI-PREVIEW-VALIDATION.md`.

### Added

- Per-user Windows Setup with selectable stable installation location, shortcuts, application update/uninstall preflight, preserved user content, and application-local Microsoft C++ runtime DLLs.
- Initial model configuration and model import from Settings, with reviewed-hash validation and no model download.
- Original layered-ring brand mark, reproducible multi-size Windows icon, and embedded desktop application version information.
- Settings & About with local model status, directory shortcuts, version and usage boundaries.
- Allowlisted portable packaging with `Open-Overglaze.cmd`, licence texts, SHA-256 file manifest, archive checksum and source dirty-state reporting. Packaging refuses existing outputs and does not publish a release.

### Preview scope

The NVIDIA model is still user-supplied and is not included or downloaded. NR stays off at each game start. This preview changes the interface and distribution workflow; it does not add game certifications or establish new in-game image-quality, performance or stability results. Existing game-specific limitations still apply.

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
