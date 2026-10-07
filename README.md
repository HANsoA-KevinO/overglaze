# 釉光 · Overglaze

[中文说明](README.zh-CN.md)

Overglaze is an experimental, third-party tool that runs NVIDIA's NR (Neural Rendering) model in DX12 single-player games and lets you control it from inside the game.

*Overglaze is the potter's technique of painting on top of a glaze that has already been fired. NR does something similar: it paints a layer on top of a frame the game has already finished rendering.*

> Overglaze is not an NVIDIA product and is not affiliated with or endorsed by NVIDIA or any game developer or publisher. It does not include the NR model; you supply it yourself.

## 0.2.0-preview.2

This preview updates the desktop game library and the in-game panel with a shared charcoal-and-green design and an original layered-ring icon. The library adds search and status filters, a selected game's next action, and expandable check and installation details. *Settings & About* shows the local model status. The panel keeps the main NR controls together and puts model read-backs and troubleshooting details under *Advanced & Diagnostics*.

The Windows Setup adds a selectable installation folder, shortcuts, update/uninstall handling and first-launch model import. Settings, models and saved captures are retained across updates; game plugins are updated separately in Game Library. See [Windows installer](docs/INSTALLER.md). The portable ZIP remains available with licence texts and file checksums. The model is still supplied by you. This preview does not add game certifications or new image-quality, performance or stability results.

## What it does

Overglaze inserts the NR model, often called "DLSS 5", into games that already use DLSS Ray Reconstruction or DLSS Super Resolution. When the game's upscaler has finished a frame, Overglaze runs NR on that frame and writes the result back. The game then continues with its own post-processing and UI.

- **NR is off by default** every time a game starts.
- **In-game panel, opened with Insert:** NR on/off, Tone, Structure, Style, Skin with AutoMask, input exposure, and a split-screen comparison.
- **The panel reports what actually happened**, not only what you asked for: which parameters the model read back, and how many frames were skipped and why.
- **Desktop program:** the Game Library checks a game, installs Overglaze's files into it, updates them and uninstalls them. Capture Browser opens compatible saved captures separately.

## What it isn't

- **Not an official integration.** Overglaze loads the model directly rather than through NVIDIA's own integration path, and its input preparation and colour handling are its own. Results are not equivalent to a game that ships NR officially.
- **Not an upscaler or frame generator.** It needs the game's own DLSS Ray Reconstruction or Super Resolution to be on, and does not add DLSS to games that lack it.
- **Not for online games or games with anti-cheat.** See [POLICY.md](POLICY.md).
- **Not a model download, and not a way to run NR on other GPUs.**
- **No guarantee of image quality.** Some games change a lot, some very little. When a game's inputs don't match what the model expects, you can see noise or ghosting.

## Requirements

- An NVIDIA GeForce RTX 50 series GPU. Tested on an RTX 5090. Other GPUs are not supported.
- NVIDIA driver 615 or newer. Tested on 617.14.
- 64-bit Windows 11. This is the only OS it has been tested on.
- Microsoft Visual C++ v14 x64 runtime. Setup and runtime-enabled ZIPs include signed application-local DLLs. Packages built without them require a separately installed runtime at least as recent as the build tools; see [Microsoft's runtime downloads](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
- A DX12 game that uses DLSS Ray Reconstruction or Super Resolution, through Streamline or by calling NGX directly. See [SUPPORTED_GAMES.md](SUPPORTED_GAMES.md).
- Your own copy of `nvngx_dlssnr.dll` (see [The model](#the-model)).
- Free disk space. Installing keeps a reserve free on the drive that holds Overglaze's data folder (currently 30 GiB).

NR is expensive. On an RTX 5090 at about 5K output, it added roughly 9–12 ms of GPU time per rendered frame in our measurements. Expect a large drop in frame rate.

## The model

This project does not include or redistribute `nvngx_dlssnr.dll`. The file belongs to NVIDIA. You supply it yourself.

Overglaze checks the file's SHA-256 against the versions it knows and refuses anything else, including modified files. See [docs/MODEL.md](docs/MODEL.md).

## Quick start

> The desktop program and the in-game panel are currently in Chinese. Labels in this README are English translations.

1. **Get the program.** Run the Setup and choose a local installation folder, or extract the whole portable ZIP. Open Overglaze from its shortcut or portable launcher.
2. **Import your model.** Select your own model DLL in the initial configuration screen or Settings. It is verified and copied into the application's models folder. Manual placement also works.
3. **Add a game.** On the Game Library page, choose *Add game* and paste the game's install folder or its EXE. A read-only check runs: store, NVIDIA modules and their signatures, anti-cheat and Denuvo markers, other injectors in the folder, and which DLSS path the game uses.
4. **Install.** Close the game. Select it in the library, choose *Prepare installation* to generate the adapter package, then choose *Install* and review the confirmation. Overglaze writes only its own files and records exactly what it wrote, so it can remove exactly those files later.
5. **Play.** Start the game the usual way and turn on DLSS: Ray Reconstruction if the game offers it, otherwise Super Resolution or DLAA. Press **Insert** to open the panel and enable NR.

### Games that need late loading

Some games refuse a proxy DLL in their folder. For those, Overglaze loads into the game after it has started, and nothing is placed in the game folder's root. You set these games up with the command-line tool `overglaze_games.exe` (see [docs/ADDING-A-GAME.md](docs/ADDING-A-GAME.md)). For Steam games, set this launch option:

```
"<Overglaze folder>\app\overglaze_launch.exe" %command%
```

The game starts as usual. The first time you press Insert in the game, Overglaze loads and opens the panel.

### Removing Overglaze

Uninstall each game from the Game Library **before** you move or delete the Overglaze folder. Also remove any Steam launch option that points to `overglaze_launch.exe`, otherwise Steam cannot start that game.

## The in-game panel

| Control | What it does |
|---|---|
| Enable Neural Rendering | Turns NR on or off. Always off when the game starts. |
| Tone (0–2, default 1) | Sets `DLSSNR.LocalToneStrength`. In our tests it mostly changes colour and tone. |
| Structure (0–2, default 1) | Sets `DLSSNR.LocalStructureStrength`. In our tests it mostly changes detail. |
| Style (0, 1, 2) | Sets `DLSSNR.Style`. The three styles are not mapped to any official names. |
| AutoMask and Skin (0–2) | Set `DLSSNR.UseAutoMask` and `DLSSNR.SkinStructureStrength`. Skin only takes effect while AutoMask is on; 0 leaves skin almost untouched. Moving Skin turns AutoMask on. |
| Input exposure | Overglaze's own colour preparation, not a model parameter. Either a manual value in stops, or automatic metering with your value as an offset. |
| Compare split | Left half shows the original frame, right half the NR result. Diagnostic view only. |
| Compute only | Runs NR without writing the result back into the game. Useful for measuring cost. |

The status card distinguishes the requested mode from the observed frame result. Expand *Advanced & Diagnostics* to see the values the model actually read during its last run, skip reasons, compute-only mode and panel settings. A parameter read-back tells you the interface took your settings; it says nothing about image quality. Structure 0 is not the same as switching NR off.

Settings other than on/off are remembered per game. The panel hotkey can be changed to F7, F8 or F9.

## Supported games

Confirmed in game:

| Game | DLSS path | Loading |
|---|---|---|
| Alan Wake 2 | Streamline · Ray Reconstruction (Super Resolution with path tracing off) | Proxy DLL |
| Halo: Campaign Evolved | NGX direct · Ray Reconstruction | Proxy DLL |
| Senua's Saga: Hellblade II | NGX direct · Super Resolution | Proxy DLL |
| CONTROL Resonant | Streamline · Ray Reconstruction | Proxy DLL |
| A Plague Tale: Resonance | Streamline · Super Resolution | Proxy DLL |
| Resident Evil Requiem | Streamline · Ray Reconstruction | Late load (Steam launch option) |

More games are listed as experimental. Notes, store versions and the meaning of "confirmed" are in [SUPPORTED_GAMES.md](SUPPORTED_GAMES.md). These are existing game-specific observations, not certification of this preview across all games. A game that isn't listed may still work. Add it in the Game Library and the check will tell you which path it uses.

## Policy in short

- Offline single-player games only. Overglaze refuses to install when it finds known anti-cheat or Denuvo markers. Not finding them doesn't prove they aren't there, so your own confirmation is what counts.
- It never patches, spoofs, debugs or dumps DRM. For four Denuvo titles, opt-in "passive coexistence" recipes only switch off Overglaze's own behaviours that could trigger the protection. You use them at your own risk.
- It never modifies NVIDIA files, and refuses model files it doesn't recognise.
- It changes no driver, system or security settings. No network access, no telemetry.
- Installs are reversible. Overglaze writes only its own files and removes them by their recorded hashes.
- Modifying a game may break its EULA or terms of service. That risk is yours.

Full text: [POLICY.md](POLICY.md).

## Documentation

- [docs/PORTABLE.md](docs/PORTABLE.md): using and building the portable preview
- [docs/MODEL.md](docs/MODEL.md): what Overglaze expects of the model file
- [docs/ADDING-A-GAME.md](docs/ADDING-A-GAME.md): adding, installing, updating and removing games
- [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md): common problems
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how it works
- [docs/CONTROL-PROTOCOL.md](docs/CONTROL-PROTOCOL.md): the local control pipe and command-line client
- [CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md), [CHANGELOG.md](CHANGELOG.md)

## Licence

Overglaze's original code is released under the [MIT licence](LICENSE). Third-party components keep their own licences; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The licence does not cover the NVIDIA DLSS SDK headers used to build it, or the model.

## Trademarks

NVIDIA, GeForce, RTX, DLSS and Streamline are trademarks or registered trademarks of NVIDIA Corporation in the U.S. and other countries. Game titles are trademarks of their respective owners. Overglaze is not affiliated with, sponsored by or endorsed by NVIDIA or any game developer or publisher. These names are used only to describe what Overglaze works with.

## Acknowledgements

- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut, for the in-game panel and the desktop program.
- [nlohmann/json](https://github.com/nlohmann/json) by Niels Lohmann.
- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu.
- [ReShade](https://github.com/crosire/reshade) by Patrick Mours. Overglaze's binding-state tracking is adapted from its state-tracking example.
- [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline), for its public headers.
- [BakingLab](https://github.com/TheRealMJP/BakingLab) by MJP, and Stephen Hill's ACES fit, used in the viewer.
