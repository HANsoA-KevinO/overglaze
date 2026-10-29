# Supported games


Overglaze works with DX12 games that use DLSS Ray Reconstruction or DLSS Super Resolution. Two paths:

- **Streamline:** the game uses NVIDIA Streamline, and Overglaze hooks Streamline's public interface.
- **NGX direct:** the game calls NGX itself, as Unreal Engine's DLSS plugin does, and Overglaze hooks the NGX evaluate call.

When a game offers both, Overglaze inserts NR after Ray Reconstruction; otherwise after Super Resolution.

**Confirmed** means the maintainers turned NR on in the game with Overglaze and saw it working: frames were processed and the effect was visible. It does **not** rate image quality or performance, and it does not cover every scene or setting. Every recipe is pinned to a specific game version. When the game updates, Overglaze refuses to load until the game has been re-checked and re-adapted (see [docs/ADDING-A-GAME.md](docs/ADDING-A-GAME.md)).

**Loading:**

- *Proxy DLL:* a small `dxgi.dll` in the game folder loads Overglaze when the game starts.
- *Late load:* nothing goes into the game folder's root. Overglaze is loaded after the game has started, by the Steam launch option or by `overglaze_games attach`.

## Confirmed

| Game | DLSS path | Loading | Notes |
|---|---|---|---|
| Alan Wake 2 | Streamline · Ray Reconstruction; Super Resolution when path tracing is off | Proxy DLL | Epic Games Store version. The game feeds unexposed scene values to the upscaler, so expect large negative exposure; automatic metering covers this. |
| Halo: Campaign Evolved | NGX direct · Ray Reconstruction | Proxy DLL | |
| Senua's Saga: Hellblade II | NGX direct · Super Resolution | Proxy DLL | Xbox app version. Display-resolution motion vectors and the letterboxed image are handled. NR together with frame generation is not yet confirmed. |
| CONTROL Resonant | Streamline · Ray Reconstruction | Proxy DLL | Set up entirely from the Games page. |
| A Plague Tale: Resonance | Streamline · Super Resolution | Proxy DLL | Xbox app version. |
| Resident Evil Requiem | Streamline · Ray Reconstruction | Late load (Steam launch option) | Carries Denuvo Anti-Tamper: passive coexistence, set up from the Game Library like any other game; the install confirmation names the risk (see [POLICY.md](POLICY.md)). The game refuses any proxy DLL in its folder, so the Game Library loads it late and shows the Steam launch option to paste. NR together with frame generation is not yet confirmed. |

## Experimental

Each of these has a recipe or a known DLSS path. Validation is incomplete; any partial real-game results are stated explicitly below. Reports are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md).

| Game | DLSS path | Loading | Notes |
|---|---|---|---|
| 007 First Light | Streamline · Ray Reconstruction | Proxy DLL | NR has run in this game during development; not yet confirmed with this release. |
| Cyberpunk 2077 | Streamline · Ray Reconstruction; Super Resolution without path tracing | — | NR has run in this game during development. No recipe for this release yet. |
| DOOM: The Dark Ages | Streamline · Ray Reconstruction | Proxy DLL | Xbox app version. Recipe only, never launched. |
| Clair Obscur: Expedition 33 | NGX direct · Ray Reconstruction | Proxy DLL | Xbox app version. Recipe only, never launched. |
| Indiana Jones and the Great Circle | Streamline · Ray Reconstruction | Proxy DLL | Xbox app version. Recipe only, never launched. |
| LEGO Batman: Legacy of the Dark Knight | NGX direct · Ray Reconstruction | Late load | Carries Denuvo Anti-Tamper: passive coexistence; the Game Library loads it late and the install confirmation names the risk. Late load on NGX direct misses the game's start-up DLSS creation: switch the DLSS mode once after loading. Never launched. |
| Phantom Blade Zero (PBZ) | NGX direct · Ray Reconstruction | Proxy DLL | Preview 14 exact-build loading recipe. Fresh-launch NR works without changing DLSS quality. One updated-candidate menu/scene check reached 5,344 evaluations / 5,343 GPU completions (one in flight), with no skips/history gaps/errors; old untyped failure cause is not uniquely established. NR+FG was reported temporarily normal. Brief functional validation only; image quality, performance and long-duration stability remain unvalidated. Remove the old Overglaze Steam option after updating the plugin. |
| Onimusha: Way of the Sword | Streamline · Ray Reconstruction | Late load | Partial Preview 12 validation: current-scene FG-on / NR-off panel stability confirmed (370 Presents/draws/GPU completions, no skips/errors). Separate FG-off NR execution and binding restoration passed; tester reported normal switch effects. Image quality, performance, long-duration stability and combined NR+FG remain unvalidated. Exact affected-build binding preservation; Denuvo passive coexistence. |
| PRAGMATA | Streamline · Ray Reconstruction | Late load | Carries Denuvo Anti-Tamper: passive coexistence; the install confirmation names the risk. Never launched. |

## Not supported

- Games without DLSS Ray Reconstruction or Super Resolution, and non-DX12 games.
- Games where DLSS is off. Turn on Ray Reconstruction, Super Resolution or DLAA in the game's settings.

## Online games and anti-cheat

Overglaze is best suited to offline single-player games. Online games and games with anti-cheat are not refused: the check names what it found, and the install confirmation asks you to acknowledge the risk. Such games may refuse to start, kick you, or penalise or ban your account. Overglaze never hides from or bypasses any protection. None of them is confirmed here, and none is listed above.

## Frame generation

Overglaze inserts NR at the game's rendered-frame upscaler return. Running NR together with DLSS Frame Generation has not been checked systematically. Preview 12 changes the shared DX12 late-panel submission path; Onimusha's current-scene panel passed with FG on and NR off. That panel result does not validate NR+FG image quality or certify other games. Preview 14 includes PBZ's brief NR+FG, no-quality-toggle launch and menu/scene checks; it does not establish long-duration or quantitative image-quality validation. If NR stops or the panel disappears after you turn frame generation on, please report it.

## Your game isn't listed?

Add it on the Games page. The read-only check tells you whether it has a usable DLSS path and whether anything blocks the install. See [docs/ADDING-A-GAME.md](docs/ADDING-A-GAME.md).
