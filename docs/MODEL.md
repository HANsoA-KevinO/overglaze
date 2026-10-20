# The NR model

## Where it comes from

This project does not include or redistribute `nvngx_dlssnr.dll`. The file belongs to NVIDIA. You supply it yourself.

Overglaze cannot give you any rights to NVIDIA software. Make sure you are entitled to use the file the way you intend.

Issues and pull requests that ask for the model, link to it, or attach it are closed.

## Where to put it

Copy the file to `app\models\nvngx_dlssnr.dll` in the Overglaze folder, next to the programs.


- Overglaze never modifies the file.
- When you install Overglaze into a game, a copy of the model goes into Overglaze's own subfolder in that game. If an identical model is already there, it is reused.
- Uninstalling removes only the copy Overglaze put there. A model that was in the game folder before is left alone.

## What is checked

Each of these checks must pass; if any one fails, NR does not start.

1. **On the desktop:** before generating a package or installing, the file's SHA-256 must match a known version (table below).
2. **At install:** the copy written into the game folder is hashed again and must match.
3. **In the game:** before loading, the in-game component checks the file's SHA-256 again. After loading, it checks the module that actually got loaded, so a different file found through the DLL search path is refused too.

Anything that doesn't match is refused. Overglaze does not try to repair or work around a mismatch.

## When a model is refused

A refused import, the model status (*Settings → 模型*, `overglaze_games model`) and the Games page say which case the file is. To tell, Overglaze reads two things from the file as data, without loading or running it:

- its **file version**, from the version resource (`GetFileVersionInfoExW`, which maps the file as data);
- its **Authenticode signature**, with `WinVerifyTrust` (no network: revocation checks off, cached URL retrieval only). It counts as NVIDIA's only when the signature is valid and the signing certificate's common name is *NVIDIA Corporation*.

Both run with a time limit (15 seconds); anything that doesn't finish, or can't be read, is reported as unknown.

| Case | What it means | What to do |
|---|---|---|
| NVIDIA-signed, newer than every known version | Most likely a newer original that this Overglaze doesn't know yet | Update Overglaze (*设置 → 检查更新*). To use it before then, turn on *允许使用未识别的模型*. |
| NVIDIA-signed, not newer | An NVIDIA-signed file that Overglaze hasn't verified | Turn on *允许使用未识别的模型* if you want to use it, at your own risk. |
| No valid NVIDIA signature | Unsigned, a broken signature or another signer: the file may have been modified (for example a version changed for other GPUs) | Overglaze uses only the original by default; turn on *允许使用未识别的模型* to use it anyway, at your own risk. |

Every message shows the file version (or 未知, unknown) and the first 12 hex digits of the file's SHA-256, so you can report it. This only explains the refusal: what is accepted is still decided by the SHA-256 table below, your opt-in and each install's pin. A valid NVIDIA signature never makes a file acceptable on its own.

## Unrecognized models (opt-in, at your own risk)

Overglaze verifies only the original model listed below. A file whose SHA-256 is not in the table is an unrecognized model. It is used only after you explicitly allow it:

- in the desktop program: **Settings → 允许使用未识别的模型** (off by default);
- on the command line: `--allow-unrecognized-model` with `import-model`, `make-package`, `refresh-package`, `install`, `update`, `migrate` or `repin`.

An unrecognized model may not run, may produce a different image from the original, or may carry security risks. Overglaze does not test or endorse any such file, and you use it at your own risk. The model status shows its SHA-256 with the tag *未识别 · 非原版 · 风险自负*, and the install and update confirmation repeats the warning.

With the setting on, the checks above still apply, to that exact file:

- importing accepts it, but never overwrites a different file already in `app\models`;
- the package generated for a game pins its SHA-256 in the game's installation config (`model_sha256`);
- in the game, the in-game component loads a known version or exactly that pinned file, nothing else. If the file changes after installation, it is refused as before.

Installations made without the setting have no pin and keep accepting known versions only. Turning the setting off again stops new imports and installs of an unrecognized model; a game already installed with one keeps the file it pinned until you update or uninstall it.

## Known versions

| File version | SHA-256 | Size (bytes) | Status |
|---|---|---|---|
| 310.8.0.0 | `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e` | 165,840,496 | Supported, tested |
| 310.9.16.0 | `2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f` | 266,424,944 | Supported, tested on the real-NR fixture |

Both known versions take the same parameters; 310.9.16.0 adds one input Overglaze does not set (`DLSSNR.MaskBlendMode`). To switch between them, move the current file out of `app\models`, import the other one, then update each game's plugin so its package carries the new hash. 310.9.16.0 also carries two networks, chosen when NR starts: network 1 is the same as 310.8.0.0's only network and network 2, a newer and smaller one, is its default; the panel's *网络* setting (自动 / 1 / 2) picks one for comparison, and switching rebuilds NR and clears its history.

Other versions are refused until they have been tested and added here, unless you allow unrecognized models (above). If you have a different version of the original, open an issue with its file version and SHA-256 only. Never attach the file.

To compute the hash in PowerShell:

```powershell
Get-FileHash .\models\nvngx_dlssnr.dll -Algorithm SHA256
```

## How Overglaze drives the model

The model is a DLL that exposes NGX-style entry points. Overglaze loads it directly, with a restricted DLL search path, instead of going through NVIDIA's own integration path. It then:

- initialises it on the game's D3D12 device;
- creates the NR feature (NGX feature id 18) at 1:1 scale, with output resolution equal to input resolution;
- sets these parameters for each frame:

| Parameter | Panel control | Range used by Overglaze |
|---|---|---|
| `DLSSNR.LocalToneStrength` | Tone | 0–2 |
| `DLSSNR.LocalStructureStrength` | Structure | 0–2 |
| `DLSSNR.Style` | Style | 0, 1, 2 |
| `DLSSNR.SkinStructureStrength` | Skin | 0–2 (only acts with AutoMask on) |
| `DLSSNR.UseAutoMask` | AutoMask | 0 or 1 |

The model accepts Tone and Structure values above 2, but in our tests results start to clip and show artefacts from about 4, so Overglaze caps them at 2.

During each run, Overglaze records which of these parameters the model actually read, and reports it in the panel and over the control pipe:

- **L1:** the model loaded.
- **L2:** NR ran.
- **L3:** the model read back Tone, Structure and Style.

This shows the interface took the values. It does not show where in the network they act, or that the image is correct.

The model's inputs and outputs (colour, depth, motion vectors) and how Overglaze prepares them are described in [ARCHITECTURE.md](ARCHITECTURE.md).

## What is not known

- The three Style values are not mapped to any official preset names.
- Exactly which part of the network Tone and Structure act on is not established.
- Results are not claimed to be equivalent to a game that integrates NR officially.
