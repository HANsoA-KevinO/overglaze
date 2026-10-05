# Troubleshooting

Before reporting a problem, collect:

- the panel's status lines (a screenshot of the panel is fine);
- the output of `overglazectl <PID> GetStatus` while the game runs;
- `nr-fault.json` from the session folder, if there is one.

Also note your GPU, driver version, the game's store and version, and whether frame generation was on. Remove personal paths before posting. Never attach DLLs.

Session records and logs are under `data\` in the Overglaze folder.

## The panel doesn't open

- **Hotkey.** The default is Insert, and it can be changed to F7, F8 or F9 in the panel. The game window must be in front.
- **Late-load games.** Overglaze loads on the first Insert press, and the panel appears once Overglaze has seen which queue presents the game's frames, about 60 frames later. If the panel disappears later, for example after the game recreated its swap chain, the evidence became contradictory and the panel was withdrawn until it is consistent again. Meanwhile NR can still be controlled with `overglazectl`.
- **Proxy-DLL games.** On the Games page, check that the game shows as installed and *healthy*. If it says *needs update* or *game changed*, update or re-adapt it first.
- **Antivirus.** Proxy DLLs are a common target for antivirus heuristics. If `dxgi.dll` or files in Overglaze's subfolder were quarantined, the health check reports modified or missing files. Restore them, or uninstall and reinstall.

## "Waiting for a usable frame" and NR never starts

- **Turn on DLSS** in the game's graphics settings. Overglaze runs after the game's upscaler: it uses Ray Reconstruction if the game has it (usually only with ray tracing or path tracing on), otherwise Super Resolution or DLAA. With DLSS off there is nothing to attach to.
- **Read the reason.** The panel shows the latest admission refusal and the latest skipped call, by name. Common ones:
  - a missing input (depth or motion vectors not provided this frame);
  - a resource format or state that NR has no path for;
  - frames on another viewport.
- **Model.** If the model is missing or its SHA-256 doesn't match, NR can't load. The panel and status say so. See [MODEL.md](MODEL.md).

## NR is on but I can't see a difference

- Turn on **Compare split** (left original, right NR) and look at faces, hair, fabric and fine texture.
- Raise **Structure**; it mostly affects detail. In our tests, Tone 0 with Structure 0 gave a result close to NR off.
- Check the **read-back line**. If Tone, Structure and Style aren't listed as read, the model didn't receive your values.
- How visible NR is depends on the game and scene. When the game's own Ray Reconstruction has already cleaned up the image, NR's change can be small.
- Check **exposure**. Some games hand the upscaler unexposed scene values, which reach NR as an almost white image. Turn on automatic metering, or lower the exposure.

## Coloured noise or speckles in dark areas

NR's change is carried back to the game as a per-channel ratio. When the frame reaches the model too dark, small edits in dark pixels become large ratios and show up as coloured noise. Turn on automatic metering, or raise the input exposure. See [ARCHITECTURE.md](ARCHITECTURE.md#colour-handoff).

## Ghosting or smearing in motion

Possible causes include motion vectors or depth that don't match what NR expects, and history that wasn't reset at a cut. Note the scene and settings and report it, together with the status output (`rejected_call`, `history_gaps`, skipped frames). Lowering Structure reduces how much NR changes the image.

## Large frame-rate drop

NR is expensive. On an RTX 5090 at about 5K output, it added roughly 9–12 ms of GPU time per rendered frame in our measurements. Frame generation doesn't hide this cost, because NR runs on every rendered frame. Use **Compute only** to measure NR's cost without changing the image. In our measurements, with NR off (including after it had been on), Overglaze's presence had no measurable cost.

## The panel shows skipped frames

A skipped frame means a check failed for that frame. The game showed its own upscaled image for it, NR stayed on, and history was reset. An occasional skip is normal: menus, loading, and resolution changes all cause them. A count that keeps rising with the same reason is worth reporting.

## NR stopped by itself

NR stops only on a fault that can't be skipped safely. The panel shows the reason, and `nr-fault.json` is written to the session folder. Restart the game to try again, and please report it with that file.

## Installation is refused

The Games page shows the reason for each refused action; hover over a disabled button to see it. Common reasons:

| Reason | What to do |
|---|---|
| Anti-cheat or Denuvo found | Not supported by default. See [POLICY.md](../POLICY.md). |
| Another injector in the folder | Remove the other tool (ReShade, another `dxgi.dll` or proxy DLL) first. Overglaze doesn't install alongside them. |
| No usable DLSS path | The game has no DLSS Ray Reconstruction or Super Resolution that Overglaze can use. |
| Unsigned NVIDIA modules | The game's Streamline or DLSS modules don't carry a valid signature. Overglaze won't hook them. |
| Game is running | Close it. |
| A file already exists | Overglaze never overwrites files it didn't create. |
| Not enough disk space | Installing keeps a reserve free on the drive with Overglaze's data folder (currently 30 GiB). |
| Model mismatch | The model in `models\`, or one already in the game folder, isn't a known version. |

## The game won't start after installing

1. Uninstall it from the Games page.
2. If the game rejects a proxy DLL, it typically crashes immediately at start, even with Overglaze otherwise idle. Use a late-load package instead (see [ADDING-A-GAME.md](ADDING-A-GAME.md#late-load-games)).
3. If you no longer have the Overglaze folder, use your store's "verify game files" function. In proxy-DLL installs, Overglaze's files in the game folder are `dxgi.dll` and its own subfolder; you can delete them by hand.

## The game changed after an update

The in-game component refuses to load into a game version it doesn't recognise. On the Games page, choose *Re-check and adapt*. See [ADDING-A-GAME.md](ADDING-A-GAME.md#when-the-game-updates).

## Steam can't start the game

If the Overglaze folder was moved or deleted, a launch option pointing to `overglaze_launch.exe` stops Steam from starting the game. Remove the launch option, or update it to the new folder.

## Xbox app games

Xbox app games run from a protected system folder that redirects to the real install folder. When you add the game, use the real install folder. Overglaze follows the redirect only when Windows confirms it belongs to that game's package. The game's EXE usually can't be read, so the game is pinned by its package identity, and some checks show as unknown.

## Driver-level DLSS overrides

If the NVIDIA driver loads newer DLSS modules from its own override location, Overglaze accepts them, provided they are validly signed by NVIDIA.

## Multiple swap chains

The panel may report that the game uses more than one swap chain. This is informational and doesn't block NR.
