# Adding a game

This page covers adding, installing, updating and removing a game, and how to contribute a recipe for a new one.

## Before you start

The game must meet all of these:

- It is a DX12 game.
- It uses DLSS Ray Reconstruction or DLSS Super Resolution, either through Streamline or by calling NGX directly.
- You play it offline, single-player, and it has no anti-cheat (see [POLICY.md](../POLICY.md)).
- It is installed on a local fixed drive.

You also need the model in place (see [MODEL.md](MODEL.md)).

## On the Games page

1. **Add game.** Open `overglaze_viewer.exe` and go to the Games page. Paste the game's install folder or its EXE path. This step only registers the game and checks it; nothing is installed.
2. **Read the check.** The read-only check reports each of the items below as passed, failed or unknown:

   | Check | What it looks at |
   |---|---|
   | Store | Steam, Epic, Xbox app (identified by the system package identity) or unknown |
   | EXE | Readable? Valid 64-bit PE? Xbox app EXEs usually can't be read; that is reported as unknown, not as passed. |
   | Denuvo | Section names in the EXE that Denuvo-protected executables carry |
   | Anti-cheat | File names of known anti-cheat systems in the game folder. The scan is bounded; if it stops early, the result is unknown. |
   | Other injectors | Other proxy DLLs or ReShade files in the game folder |
   | NVIDIA modules | Streamline and DLSS modules: version, SHA-256, Authenticode signature |
   | Route | `sl-rr`, `sl-sr` or NGX direct; Ray Reconstruction is preferred over Super Resolution |

   A failed Denuvo, anti-cheat or injector check blocks the install. Unknown results don't block by themselves, but they mean Overglaze couldn't check that item, so you have to know the answer yourself.
3. **Generate the adapter package.** The package pins the game version (the EXE hash, or the package identity for Xbox app games) and the hashes of its NVIDIA modules, and bundles the current in-game files. The default loading method is the proxy DLL.
4. **Install.** Close the game first. Confirm that it is played offline, single-player, without anti-cheat. Overglaze then:
   - stages and checks the files;
   - writes them, the proxy DLL last;
   - runs the install checker;
   - records the transaction.

   If any step fails, the files written so far are removed, and you are told which step failed.
5. **Play.** Start the game normally. In its graphics settings, turn on DLSS: Ray Reconstruction if available (usually needs ray tracing or path tracing on), otherwise Super Resolution or DLAA. Press **Insert**, and tick *Enable Neural Rendering*.

### What to look at in the panel the first time

- **Waiting for a usable frame** means Overglaze hasn't admitted a frame yet. The panel says why: usually DLSS is off, or the game uses a DLSS path that is off in its settings.
- **Evidence level L3** means the model read back Tone, Structure and Style. That is a good sign that the wiring is right.
- **Skipped frames** with a named reason are normal now and then. The game shows its own image for those frames. If the count rises constantly, note the reason and report it.
- **Compare split** shows the original on the left and NR on the right, which helps judge whether NR is doing anything.

## From the command line

`overglaze_games.exe` uses the same game manager as the Games page. Every command prints JSON. Run it with `--help` for the full list.

```
overglaze_games preflight "<game EXE or folder>"      # read-only check, registers nothing
overglaze_games add "<game EXE>"                      # register and check
overglaze_games list                                  # every registered game with status
overglaze_games make-package <id> [options]           # generate the adapter package
overglaze_games plan-install <id>                     # dry run: files, sizes, hashes, space checks
overglaze_games install <id> --offline --no-anticheat --consent "<your note>" [--progress]
overglaze_games health                                # do installed files still match their packages?
overglaze_games update <id> --offline --no-anticheat --consent "<your note>"
overglaze_games repin <id> --offline --no-anticheat --consent "<your note>"
overglaze_games uninstall <id> --confirm
overglaze_games storage                               # recovery copies and staging kept per game
```

`--offline`, `--no-anticheat` and `--consent` are your explicit confirmation. They are not optional, and Overglaze records them.

Useful `make-package` options:

| Option | When to use it |
|---|---|
| `--late` | The game refuses a proxy DLL in its folder: it crashes at start with only Overglaze's `dxgi.dll` present. Overglaze then loads after the game starts. |
| `--root-proxy-on-insert` | The proxy DLL only forwards until the first Insert press, and then loads Overglaze. |
| `--binding-preservation` | The game crashes in the graphics driver right after NR first runs. This usually means the game keeps recording commands after the upscaler without setting its state again. With this option, Overglaze restores the game's bindings after inserting NR. |
| `--exposure <stops>` | Default input exposure for the panel. Automatic metering usually makes this unnecessary. |

On the Streamline route, the viewport and depth type are read from the game's own calls at run time. You normally don't need `--viewport` or `--linear-depth`.

## Late-load games

1. Generate the package with `--late`, and install it as usual. Only Overglaze's own subfolder is written; nothing goes into the game folder's root.
2. Load Overglaze into the running game in one of these ways:
   - **Steam:** set the launch option

     ```
     "<Overglaze folder>\overglaze_launch.exe" %command%
     ```

     The launcher starts the game unchanged. The first time you press Insert with the game in front, Overglaze loads and the panel opens. For a game without a late-load package, the launcher just starts it.
   - **Other launchers:** `overglaze_games watch` waits for late-load games to start and loads Overglaze into each one once. Or, with the game running, use `overglaze_games attach <id> --pid <PID>`.
3. The panel appears after Overglaze has seen which queue presents the game's frames, which takes about 60 frames. Until then the panel stays hidden.

## When the game updates

Recipes pin the game version. After an update, the Games page shows that the game has changed and needs re-adaptation, and the in-game component refuses to load into an unknown version. Choose *Re-check and adapt* (command line: `repin`). This:

- runs the full check again;
- uninstalls the old files, keeping a recovery copy;
- moves the old package aside (it is not deleted);
- generates a new package and installs it.

If the check now fails, for example because the route changed or a module is no longer signed, nothing is touched. Some built-in recipes can't be re-adapted automatically. If that happens, open an issue with the check output.

When Overglaze itself updates, already-installed games show *update available*. Each one is updated separately, and only while that game is closed.

## Removing a game

On the Games page, choose *Uninstall*, or run `overglaze_games uninstall <id> --confirm`.

- Only files whose SHA-256 still matches the install record are removed.
- A recovery copy is kept first.
- A model that was in the game folder before Overglaze is left alone.
- Remove any Steam launch option that points to `overglaze_launch.exe`.

If a file is in use, the uninstall stops and the record is kept, so you can retry after closing the game.

## Contributing a recipe

The JSON below is illustrative; the program writes the exact format.

A recipe is the game facts that let others install Overglaze into the same game version. A pull request for a new game includes three things.

**1. The game-facts JSON** in `data/games/<id>.json`. It must not contain any local path. Illustrative shape:

```json
{
  "id": "example-game",
  "title": "Example Game",
  "executable": "ExampleGame.exe",
  "store": "steam",
  "route": "sl-rr",
  "loader": "root_proxy_d3d12",
  "pins": {
    "ExampleGame.exe": "<sha256>",
    "sl.interposer.dll": "<sha256>",
    "sl.common.dll": "<sha256>",
    "sl.dlss_d.dll": "<sha256>"
  },
  "facts": {
    "binding_preservation": false,
    "linear_depth": false,
    "viewport": 0,
    "default_exposure_stops": 0.0
  },
  "denuvo_passive_coexistence": false
}
```

**2. The check report:** the output of `overglaze_games preflight "<game EXE>"`. Replace your user name and folder names in any path.

**3. Test notes:**
- store and game version;
- DLSS mode, and whether frame generation was on or off;
- what the panel showed: route, evidence level, skipped frames and their reasons;
- whether NR was visibly working.

See [CONTRIBUTING.md](../CONTRIBUTING.md) for what is not accepted, such as games with anti-cheat, DRM workarounds and attached binaries.
