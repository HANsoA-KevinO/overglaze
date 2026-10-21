# Changelog

All notable changes to Overglaze are listed here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed

- Importing a model now replaces a different model already in `app\models` instead of refusing with *模型目录已有不同文件；请先核对该文件，不会自动覆盖*, so switching between 310.8.0.0 and 310.9.16.0, or replacing a model, is just *导入模型*. The new file must pass the same checks as before (a known version, or an unrecognized one with *允许使用未识别的模型* on); a refused file never replaces anything, and importing the same file again changes nothing. Before anything is replaced, the outgoing file is copied by handle into Overglaze's own model store (`data\settings\plugin-manager\_models\<SHA-256>\nvngx_dlssnr.dll`, the store recovery copies already share) and verified by SHA-256, unless an identical copy is already there. The new file is then written beside the old one under a temporary name, flushed and verified, and renamed over it; on any failure the old file stays in place. The model setup screen says what happened, for example *已更换模型：310.8.0.0 → 310.9.16.0。已装游戏要在游戏库点一次「更新插件」才会用上新模型；原来的模型已保存在釉光的模型库中，随时可以重新导入。*, and where the old file is kept. `overglaze_games import-model` reports it as `replaced` and `replaced_text`. No model is loaded or executed during an import.
- Installed games keep their own copy of the model and keep working after a switch. The Game Library now shows *有更新* for each of them, with *釉光的模型已换成 …，这款游戏还在用 …* and *更新插件* as the way to move it to the new model (the update refreshes the package first, as for a newer plugin). A game without the plugin whose package was made for the previous model shows *适配包需要刷新* (*刷新适配包*) instead of the blocked *Lab 的本地 NR 模型与适配包清单不一致*. An update that needs the model and finds none usable in `app\models` is refused before anything is uninstalled.
- The docs no longer tell you to move the model out of `app\models` by hand; see *Switching models* in [docs/MODEL.md](docs/MODEL.md#switching-models).
- A Game Library refresh no longer hashes the same files again for every game. The status checks (each game's status, the model status, the published plugin) keep the SHA-256 of each file they read in memory, keyed by the file's identity: volume serial and file index, size, last-write time and change time. A file is hashed again only when one of those changes, so a file replaced or rewritten behind Overglaze's back is still noticed; an import also drops what was kept for the model's path. Install, uninstall, update, packaging and import still hash every file they act on themselves. On a synthetic library of 11 installed games with a 254 MiB model, a refresh took about 2.6 s each time before; now the first refresh in a session hashes each file once (about 1.4–1.5 s) and each later one takes about 0.23 s.

## [0.2.0-preview.9] - 2026-10-21

### Added

- The in-game panel's hotkey can be changed. It is one setting shared by every game, Insert by default: one key with optional Ctrl, Shift or Alt, for example Ctrl+F10. Change it in the desktop program under *设置 → 游戏内面板 → 面板快捷键*, or in the panel under *高级与诊断 → 面板设置*: *修改* waits for the next key (Esc cancels), *恢复默认（Insert）* goes back. Keys the panel needs for its own controls, and keys that would type text into it, are refused with the reason shown: Esc, Enter, Space, Tab, the arrow keys, Backspace, Delete, Home and End; letters and digits without Ctrl or Alt; Ctrl+A / C / V / X / Y / Z; Ctrl+Alt with a letter or digit (AltGr on many layouts); Alt+F4; Ctrl+Pause and Ctrl+Scroll Lock (they arrive as Break); mouse buttons; the Windows keys; a modifier alone. Allowed keys are F1–F24, Insert, Pause, Scroll Lock, Page Up and Page Down, and, with Ctrl or Alt, letters, digits and keypad digits. The modifiers must match exactly: with Ctrl+F10, F10 alone is the game's.
- The setting is `data\settings\panel-hotkey-v1.json`, written atomically: `{"kind": "overglaze-panel-hotkey", "version": 1, "key": "Ctrl+F10"}`. A missing or malformed file, or one that names a refused key, means Insert; the panel and Settings say when the file was malformed. A running game picks up a change made in the desktop program within about a second, and the desktop program follows a change made in a game's panel. The Steam launcher (`overglaze_launch.exe`) waits for the configured key before it loads Overglaze into a late-loading game, and the root proxy's load-on-first-press mode (`root_proxy_on_insert`) reads the same file through the installation's data root and looks at it again every two seconds.
- Every text that names the key shows the configured one: the panel's footer, the Game Library's game page and Steam launch-option card, the model setup screen, the launcher's usage message, and the texts of `overglaze_games` (including `watch`). `overglazectl <PID> GetStatus` reports the key and where it came from under `host.overlay` (`hotkey`, `hotkey_source`) and, for the root proxy's load-on-first-press mode, under `host.root_proxy`.

### Changed

- The panel's earlier per-game choice of Insert, F7, F8 or F9 (面板设置, stored as `hotkey` in each game's `overlay-<package>.json`) is no longer used; the field is left in that file as it was. If you had chosen F7, F8 or F9, set it once more in the new setting.
- When the panel opens with a hotkey that includes Ctrl or Shift, the game window now receives the release of those modifiers, so a game that saw them go down does not keep them held while the panel owns the keyboard. The hotkey's own press, repeats and release go to the panel only.
- **Plugin update needed.** The in-game host (`overglaze\overglaze_controller.dll`) and the root proxy (`dxgi.dll`) changed. An installed game keeps the fixed Insert key until it is updated with *更新插件* in the Game Library.

### Fixed

- A new version found by the update check was shown only in that session. After *稍后* or closing the program, the notice did not come back until the next automatic check, up to 24 hours later. The program now remembers a signed update it found in `data\settings\update-offer-v1.json` (the exact signed manifest and signature, the release tag and page, and the Setup's download address; at most 512 KiB, written atomically). Every start verifies it again with the built-in release key, without connecting, before and independent of the 24-hour check, and shows the notice at once while that version is newer than the running one. If the file fails verification or names a version that is not newer, it is removed and the reason is logged. *稍后* hides the notice for the current session only. *更新* on a remembered offer takes the same path as after a fresh check: download, size and SHA-256 against the signed manifest, and the same checks for running games and disk space. If the remembered address fails, the program checks again and shows the reason. A check that finds a newer signed release replaces the file. A check that finds nothing newer, or only a release without a signed manifest, removes it. `overglaze_games self-update check` updates the file in the same way. While a check started at launch is still running, *更新* waits for it (*正在检查更新…*).
- When a copy was treated as portable because the Windows uninstall entry gave no install location, the reason did not say why. It now carries the Win32 error and tells the cases apart: *Windows 卸载登记里没有这个程序（错误 2）* when the Setup's uninstall entry is missing, *Windows 卸载登记里没有安装位置（错误 2）* when only its `InstallLocation` value is, *安装位置类型不对（错误 1630）* for a value of the wrong type, and *无法读取 Windows 卸载登记（错误 N）* for any other error. It appears where the update notice explains that the copy is portable.

## [0.2.0-preview.8] - 2026-10-21

### Changed

- Other injectors beside the game EXE (`dxgi.dll`, `d3d12.dll`, `d3d11.dll`, `version.dll`, `winmm.dll`, `dinput8.dll`, `ReShade.ini`) no longer block packaging or installing. They are a named risk notice, like anti-cheat and anti-tamper: the game card's *其他注入工具* tag, a line on the game's page and in the install / update / re-adaptation confirmation (*检测到其他注入工具：version.dll。可能和釉光冲突；如果游戏崩溃或 NR 不工作，先移除它再试。*), a failed `other-loaders` check, and `other_loaders` in the status and preflight JSON. Installing is the acknowledgement; the transaction and the install receipt record `other-loaders` in `risks` with the file names in `other_loaders`. Overglaze never changes or removes the other tool's files. Two cases still refuse, because installing would do real harm, and the refusal names the file and why: another DLSS 5 / NR add-on (`renodx-dlss5.addon64`, `overglaze*.addon64`, `dlsslab*.addon64`; two NR passes would stack), and a `dxgi.dll` that is not Overglaze's beside the EXE for a package that loads through the root proxy (Overglaze would have to put its own file there and never overwrites one). With late loading, which writes nothing at the root, a `dxgi.dll` there is only a notice. An installed game with another NR add-on beside it is no longer offered *Update plugin*, and `update` refuses before uninstalling anything. The install refusal message now carries the status's reason instead of a generic text. The state label is now *插件冲突*.
- The model status (`overglaze_games model`, Settings → 模型) adds `file_version`, `nvidia_signed`, `signer`, `classification` (`known`, `nvidia-newer`, `nvidia-unverified`, `not-nvidia`, `unknown`) and `explanation` / `explanation_text`, and lists each known version's `file_version`. What is accepted is unchanged: the SHA-256 table, *允许使用未识别的模型* and each install's pin.

### Fixed

- Importing a model that is not a known version said only "模型版本未识别" and nothing about why. Before refusing, Overglaze now reads the file's version resource and checks its Authenticode signature (as data, never loading the file; no network: revocation off, cached URL retrieval only; leaf signer must be NVIDIA Corporation; at most 15 seconds, anything unfinished is unknown) and says which case it is: a newer NVIDIA original than this Overglaze knows (update it in *设置 → 检查更新*, or opt in to use it now), an NVIDIA-signed file Overglaze has not verified (opt in at your own risk), or a file without a valid NVIDIA signature, which may have been modified, for example for other GPUs (opt in at your own risk). Each message includes the file version (or 未知) and the first 12 hex digits of the SHA-256. Settings → 模型 and the Games page show the same explanation for the model in `app\models`.
- A foreign `dxgi.dll` beside the EXE of a root-proxy game was found only in the install's staging step, after Overglaze's subfolder had been created in the game folder, and reported as "目标文件已存在". The check now refuses it, naming the file, before anything is written.
- *从列表移除* (`forget`) was refused whenever any `dxgi.dll` sat beside the game EXE, so a late-loading game installed next to another tool's `dxgi.dll` could be uninstalled but not removed from the library. It is now refused only while Overglaze's own plugin is still installed: an install record that is not fully removed, Overglaze's config or bridge in the game folder, or a `dxgi.dll` whose SHA-256 is one of Overglaze's (the install record's, the package's or a published host's). The message says which case applies. Another tool's `dxgi.dll` is left untouched.

## [0.2.0-preview.7] - 2026-10-20

### Added

- One-click update of the desktop program. With *启动时检查更新* on (Settings, the default) the program checks the project's GitHub releases when it starts, at most once every 24 hours; *检查更新* checks at once. A newer release shows a notice with its version and release notes and *更新* / *稍后* / *打开下载页*. *更新* downloads the Setup into `data\updates\<version>\` with progress and *取消*, checks it, starts it silently in the same folder and closes the program; the Setup waits for it (`/WAITPID`, up to 60 s) and reopens it afterwards (`/RELAUNCH`), which then shows *已更新到 <version>* once and how many installed games need a plugin update. Plugins are never updated automatically. The update is refused, with the reason, while a registered game or another Overglaze program runs, while a game plugin operation is unfinished, or when disk space is short. Only copies installed by the Setup are updated; portable copies show the new version and *打开下载页*. `overglaze_games self-update check|download` does the same check, and the download and verification without starting the Setup.
- Signed updates. Each release that can be installed this way carries `overglaze-update.json` (versions, release notes, sizes and SHA-256 of the Setup and the ZIP) signed with the project's release key (ECDSA P-256), and `overglaze-update.json.sig`. The program has the public key built in, verifies the signature over the exact bytes before reading the manifest, requires the manifest's version to match the release tag, and installs a Setup only when its size and SHA-256 match. A release without these files is offered as a manual download. New `Sign-Release.ps1` writes and verifies them; the public key is `assets/release-key/overglaze-release-p256.public.blob`.

### Changed

- Network: the desktop program now connects to GitHub to check for updates. It reads public release data only and sends nothing about you, your games or your files; turn off *启动时检查更新* to stop the automatic check. The in-game components still never connect. POLICY.md, the READMEs and docs/INSTALLER.md say so instead of "no network access".
- The installer accepts `/WAITPID=<pid>` and `/RELAUNCH`. Its existing refusals are unchanged; a process running from the program's own `data\updates\` folder is accepted only when it is the Setup that runs the check.

## [0.2.0-preview.6] - 2026-10-20

### Fixed

- Adding a game by folder now lists an Unreal game EXE that is not named `*-Shipping.exe` (an Xbox app build may call it `<Name>.exe` under `<Project>\Binaries\WinGDK`) first, like a shipping build; EXEs in the engine's or a plugin's tree do not count. Crash reporters (`crashpad_handler`, ...), launch helpers (`gamelaunchhelper`), anti-cheat launchers and setups (`start_protected_game`, Easy Anti-Cheat, BattlEye) and uninstallers are listed last, so the preselected entry is no longer a crash reporter. Choosing such a launch helper at the game's root offers the game EXE first, as for an Unreal bootstrap.
- The check now also looks for anti-cheat files at the game's root (the folder with the package manifest within the four folders the package identity looks at, or an Unreal game's root folder), two levels deep, not only around the EXE. An Xbox app package keeps Easy Anti-Cheat and `start_protected_game.exe` (now recognised as Easy Anti-Cheat's launcher) beside its manifest, several folders above the game EXE, so the install confirmation showed no anti-cheat risk for such games. Files found there are listed relative to that root and each file once.

## [0.2.0-preview.5] - 2026-10-19

### Added

- NR model 310.9.16.0 is a known version (SHA-256 `2bc19bc0…28789f`), next to 310.8.0.0. Its exports and parameter names match 310.8.0.0 apart from one new input Overglaze does not set; the real-NR fixture passes in all four variants on driver 617.14.
- NR network setting (panel *网络*: 自动 / 1 / 2, beside Style; `SetNrSettings` `network`, integer 0–2, default 0). It sets `DLSSNR.Hint.Render.Preset`, which the model reads only when the NR feature is created; 自动 (0) passes the model's own default, as before. In 310.9.16.0, network 1 is the 310.8.0.0 network and network 2 the newer, smaller one the model uses by default, so the two can be compared inside the same file. Changing it while NR runs rebuilds the NR feature at the next frame, through the same path a resolution change takes: the game shows its own image for a few frames, NR history starts fresh and NR resumes by itself. The panel's hint follows the loaded model; with 310.8.0.0, which has one network, the setting is disabled, and for an unrecognized model the meaning of the numbers is unknown. `nr_runtime.settings.applied_network` reports the network the current feature was created with, and `nr_runtime.model_sha256` the model the bridge loaded. The panel remembers the setting per game (panel preferences version 5; earlier files load with 自动).
- Adjustable compare split (panel *分界位置* under *对比分屏*, 0–100% of the width, default 50%, with a *居中* button; `SetNrSettings` `compare_split_position`, number 0–1, default 0.5). The original is shown left of round(position × width) and NR from there on, now with a thin neutral grey divider across the boundary (max(1, width / 1920) columns, about 2 px at 4K; none at 0% or 100%, where the whole frame is NR or the original). The divider's grey is the half level of Overglaze's NR input encoding taken back through the input exposure, so it stays light grey at any scene brightness. The position is applied between frames by the composite only: it never rebuilds the NR feature, resets history or changes the model's input. The slider sends its value while it is dragged, like the other sliders (at most every 100 ms, and once more on release). The toggle now reads 对比分屏：左侧原图 / 右侧 NR. `nr_runtime.settings.applied_compare_split_position` reports the position the composite held. The panel remembers the position per game, not the split itself (panel preferences version 6; earlier files load with 50%).

### Changed

- Live ABI 27 between the in-game host and the bridge (26: the settings carry the network; the status carries the network the feature was created with and the loaded model's SHA-256. 27: the settings carry the compare split position; the status the position applied). Both ship in the same package; installed games pick it up with *Update plugin*.

## [0.2.0-preview.4] - 2026-10-09

### Added

- Edit extrapolation (panel *外推* with a *倍数* slider, ×1.0–×4.0, default ×2; off by default; `SetNrSettings` `extrapolate` / `extrapolate_factor`). NR still runs once per frame and keeps its own output as history; what is written back is that pass's change multiplied: per channel in the NR API domain, N' = clamp(C + n·(N − C), 0, 1), with C the encoded image handed to NR and N its output, before the usual ratio transfer. Off, or ×1, is exactly the previous composite. It stands in for a second NR pass without a second evaluation, and is outside what the model was designed for: sharpening overshoot and fine grain are amplified too. `nr_runtime.settings` reports `applied_extrapolate` and `applied_extrapolate_factor`. The panel remembers both per game (panel preferences version 4; earlier files load with extrapolation off).

### Changed

- Denuvo Anti-Tamper and anti-cheat are now risks the user is told about, not refusals. The check still names what it found: Denuvo sections in the EXE, and anti-cheat files by the product their names point to (Easy Anti-Cheat, BattlEye, EQU8, Vanguard, Anti-Cheat Expert) or by file name. Games with either get the ordinary states and actions, with a neutral tag on the Games page. Other loaders in the game folder and an unidentifiable store package are still refused.
- One risk acknowledgement replaces the offline / no-anti-cheat declaration. In the desktop program, the install, update and re-adapt confirmation lists what the check found and states that online or anti-cheat games may not start, may kick you or penalise or ban your account, and that Overglaze never hides from or bypasses any protection; choosing *Install* or *Update* is the acknowledgement. There is no extra checkbox. On the command line it is `--accept-risk`; `--offline --no-anticheat` and `--denuvo-passive-coexistence` remain accepted as aliases. The transaction records the acknowledgement and the risks found.
- Games whose EXE carries Denuvo are set up from the Games page like any other game. *Generate adapter package* gives them late loading (nothing in the game folder's root; Resident Evil Requiem crashes with any `dxgi.dll` there) and shows the planned loading method beforehand. Every other game keeps the proxy DLL.
- After installing a late-loading game, the Games page shows the exact Steam launch option, built from the program's own folder, with a copy button and where to set it in Steam; it stays in the game's details. Games not started from Steam get the `overglaze_games watch` command instead. Uninstalling such a game reminds you to remove the launch option.
- Automatic exposure (panel *自动测光*, `exposure_auto`) now prefers the game's own exposure: the exposure texture, pre-exposure and exposure scale the game hands DLSS (Streamline exposure tag or NGX `ExposureTexture`), applied as log2(E × scale / pre-exposure). The value is read on the GPU with one frame of latency and only trusted while the game-exposed log-average lies in 2^−7.19 … 2^−0.42, with hysteresis. When the game passes none (for example 007 First Light, or NGX features created with DLSS AutoExposure such as Halo: Campaign Evolved) or its value is implausible, Overglaze's own metering is used as before. The EV slider stays an offset either way; manual exposure is unchanged. The panel shows the source on one line under the exposure controls, and `nr_runtime.settings` reports `exposure_source`, `exposure_fallback` (by name), the game's raw values and per-reason counts (see [CONTROL-PROTOCOL.md](docs/CONTROL-PROTOCOL.md#automatic-exposure)).
- Live ABI 25 between the in-game host and the bridge (the frame carries the game's exposure, the status its source; the settings carry edit extrapolation, the status the factor applied). Both ship in the same package; installed games pick it up with *Update plugin*.
- New setting **允许使用未识别的模型** (Settings; `--allow-unrecognized-model` on the command line for `import-model`, `make-package`, `refresh-package`, `install`, `update`, `migrate` and `repin`), off by default. Overglaze verifies only the original model; with the setting on, a model whose SHA-256 is not a known version can be imported (never over a different file already in `app\models`), packaged and installed. The model status shows its SHA-256 with *未识别 · 非原版 · 风险自负* and a one-line warning — an unrecognized model may not run, may look different from the original, or may carry security risks — which the install and update confirmation repeats. With the setting off, nothing changes. The known-version table still lists the original model only. README and docs/MODEL.md now say the program is tested only on the RTX 50 series and that other GPUs need a model that runs on them, as an explicit opt-in at your own risk.

### Fixed

- The desktop window is sized and centred for the monitor it opens on, at that monitor's scale. Opened on a 200% monitor it used to get a 100% size and its content overlapped until the window was resized.
- The add-game, plugin confirmation, launch option, model setup and settings dialogs no longer shrink a pixel per frame at fractional scales such as 125%.
- Adding a game by folder works for large games and Xbox app packages. The scan skips an Unreal project's asset folders (Content beside Binaries, Paks, Movies, Saved, ...), walks an Xbox package's root Content folder, lists the EXE beside DLSS modules first and selects it, still offers what a partial scan found, and says so when it finds no EXE. Choosing an Unreal bootstrap EXE at a game's root offers the game's own `<Project>-Win64-Shipping.exe` first; for a bootstrap already in the library, the check names the EXE to add instead.
- NGX direct: render-resolution motion vectors padded differently from depth (Wuthering Waves: 1705-wide motion beside 1708-wide depth, render subrect 1705) were refused on every frame, so NR stayed at *waiting for a usable frame*. Motion that covers the render region and is no larger than the depth resource is now accepted; both are cropped to the region.

### Installation configuration

- Packages generated for the controller now write configuration version 4: the user's risk acknowledgement and the facts the check found (`risk`: `acknowledged`, `anti_tamper`, `anticheat`) replace version 3's `offline_single_player` / `no_anticheat`, so a game with anti-cheat is never recorded as having none. The in-game host and the install checker accept versions 3 and 4, so plugins installed by earlier versions keep working unchanged. Research-track packages still write version 3 and are refused for games with anti-cheat files.
- After updating the application, installed games show *update available*; *Update plugin* rewrites their package with version 4. Until then they keep working as installed.
- A version 4 config may carry `model_sha256`: the SHA-256 of an unrecognized model the user allowed, written only when the package is made with one. The in-game bridge then loads a known version or exactly that file, nothing else, and the host checks the pin against the adapter package's own model hash. Configs without the key, including every one written before it existed, accept known versions only, as before. The in-game status reports the pin as `nr_runtime.unrecognized_model_sha256`.

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
