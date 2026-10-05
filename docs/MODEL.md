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

Anything that doesn't match is refused. That includes modified, re-signed or patched files, among them files altered to run on GPUs other than the RTX 50 series. Overglaze does not try to repair or work around a mismatch.

## Known versions

| File version | SHA-256 | Size (bytes) | Status |
|---|---|---|---|
| 310.8.0.0 | `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e` | 165,840,496 | Supported, tested |

Other versions are refused until they have been tested and added here. If you have a different version, open an issue with its file version and SHA-256 only. Never attach the file.

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
