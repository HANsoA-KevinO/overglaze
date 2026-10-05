# Policy

This page says what Overglaze will and won't do, and where the limits of its own checks are. It is not legal advice.

## Offline single-player games only

Overglaze is meant for DX12 single-player games played offline. It must not be used in online or competitive modes, or in any game protected by anti-cheat software.

## Anti-cheat and anti-tamper checks

Before installing, Overglaze runs a read-only check of the game:

- **Anti-cheat:** it scans the game folder for file names of known anti-cheat systems (for example Easy Anti-Cheat, BattlEye, EQU8, Vanguard). If it finds one, it refuses to install.
- **Denuvo Anti-Tamper:** it reads the game EXE's section table and looks for section names that Denuvo-protected executables are known to carry. If it finds them, it refuses to install unless you explicitly opt in to a passive-coexistence recipe (see below).
- **Other injectors:** it looks for other proxy DLLs and injection tools in the game folder (for example ReShade or another `dxgi.dll`, `d3d12.dll`, `version.dll`, `winmm.dll` or `dinput8.dll`). It does not install alongside them.

**Not detected does not mean not present.**

- Some EXEs can't be read at all, such as games installed through the Xbox app. For those games the Denuvo check returns "unknown", not "passed".
- The folder scan is bounded in size and time. If it stops early, the anti-cheat check returns "unknown".
- Marker lists are incomplete by nature. Protection that leaves no files in the game folder can't be found this way.

So the checks can only refuse; they can never certify a game as safe. **Your explicit confirmation governs.** Installing requires you to confirm that the game is played offline, single-player, and without anti-cheat. On the command line this means passing `--offline --no-anticheat --consent "<text>"`. If you are not sure, don't install.

## DRM

Overglaze never patches, spoofs, debugs, dumps or otherwise interferes with DRM or anti-tamper software, and never modifies a game's EXE or other game files.

### Denuvo passive-coexistence recipes

The repository includes recipes for four titles that carry Denuvo Anti-Tamper:

- Resident Evil Requiem
- LEGO Batman: Legacy of the Dark Knight
- Onimusha: Way of the Sword
- PRAGMATA

"Passive coexistence" means Overglaze avoids those of its own behaviours that could trigger the protection, and does nothing to the protection itself:

- The in-game component installs no exception handlers of its own.
- Where a game rejects a proxy DLL in its folder, the recipe loads Overglaze after the game has started instead. Overglaze does not rename itself or otherwise disguise a proxy DLL to get past a game's checks.

These recipes are never used by default. The desktop program refuses these games. The command-line tool accepts them only with the explicit flag `--denuvo-passive-coexistence`, and you have to give the flag again for every package, refresh, update and re-adaptation. The flag does not relax any identity, signature or state check. It only records that you knowingly chose to go ahead. **You use these recipes at your own risk.**

## NVIDIA software

- Overglaze never modifies NVIDIA modules: not the game's Streamline or DLSS DLLs, not the driver, not the model.
- The game's NVIDIA modules are checked for a valid Authenticode signature. The in-game component refuses to hook Streamline modules that aren't validly signed.
- The model is accepted only if its SHA-256 matches a known, unmodified NVIDIA release. Modified or re-signed copies are refused, including patched files meant to run on other GPU generations. See [docs/MODEL.md](docs/MODEL.md).
- Overglaze does not get around driver or GPU-architecture restrictions.

## System

Overglaze does not change driver settings, driver profiles, Windows security settings, file permissions, or the registry.

## Network and privacy

Overglaze makes no network connections. It has no telemetry, no update check and no crash upload. Logs and diagnostic files stay on your disk. You decide whether to attach any of them to a bug report; remove personal paths first.

## Reversible installs

- Overglaze writes only its own files into a game folder: in proxy-DLL installs, one `dxgi.dll` in the game folder plus its own subfolder; in late-load installs, only its own subfolder. It refuses to overwrite any file that is already there.
- Every install is recorded as a transaction with the SHA-256 of each file written.
- Uninstall removes only files whose SHA-256 still matches the record, and keeps a recovery copy first. A model that was already in the game folder before Overglaze was installed is left alone.
- A failed install rolls back the files it wrote, and tells you at which step it stopped.
- NR is off every time a game starts.

## Your responsibility

Modifying how a game renders may break the game's end-user licence agreement or terms of service, or the terms of the store you bought it from. You are responsible for deciding whether you may use Overglaze with a given game, and for whether you are entitled to use the model file you supply. Overglaze comes with no warranty (see [LICENSE](LICENSE)).

## Reporting

If Overglaze installed into a game it should have refused, or did something on this page it says it won't, please open an issue. If it's a security problem, follow [SECURITY.md](SECURITY.md) instead.
