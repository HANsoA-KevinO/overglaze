# Architecture

This page describes how Overglaze gets into a game, how it finds the frame to work on, how it runs NR, and how colour is handed to the model and back.

## Programs and files

| Piece | Runs where | Job |
|---|---|---|
| `overglaze_viewer.exe` | Desktop | Desktop program. The Games page adds, checks, installs, updates and uninstalls games. |
| `overglaze_games.exe` | Desktop | Command-line front end to the same game manager. |
| `overglaze_launch.exe` | Desktop | Steam launch-option launcher for late-load games. |
| `overglazectl.exe` | Desktop | Client for the local control pipe ([CONTROL-PROTOCOL.md](CONTROL-PROTOCOL.md)). |
| Install checker | Desktop | Runs the in-game component's own installation check against freshly installed files; if the check fails, the install is rolled back. |
| Proxy `dxgi.dll` | Game | Proxy-DLL installs only. A thin forwarder that the game loads in place of the system DXGI. |
| Controller host | Game | Hooks, in-game panel, control pipe, admission of frames, binding preservation. |
| Bridge | Game | The NR runtime: the model session, colour, depth and motion preparation, exposure metering, GPU completion tracking. |
| Model | Game | The user's `nvngx_dlssnr.dll`. |
| Install record | Game | Game facts and file hashes. The in-game host trusts it only through a hash chain anchored in the adapter package. |

In a game folder, everything except the proxy `dxgi.dll` lives in Overglaze's own subfolder.

## Source tracks

The source is split into tracks, and dependencies are only allowed in one direction. A test (`track_boundaries`) enforces this.

| Track | Contents | May depend on |
|---|---|---|
| core | Control protocol, named pipe, platform and file identity, installation contract, panel state | core |
| runtime | NR session, evaluate pipeline, colour/depth/motion codecs, exposure, completion timeline, submission and binding preservation, live bridge | core, runtime |
| provider | Streamline boundary and resource leases, RR/SR option structures, adapter, Streamline-to-NR frame translation | core, runtime, provider |
| viewer | Image library, preview and export, viewing settings | core, viewer |
| controller | In-game panel, game manager, command-line tools, desktop program, host, proxy, NGX observer, late-load support | all of the above |

Image capture, replay and diagnostic probes are not part of this release.

## Getting into the game

### Proxy DLL (default)

The installer puts a thin `dxgi.dll` in the game folder.

- It imports only from `kernel32`, and forwards every DXGI export to the system `dxgi.dll`.
- Its exports never call into the Windows loader. Windows compatibility shims may call DXGI exports while the game's imports are still being resolved, and the loader is not safe to re-enter at that point.
- The host is loaded from Overglaze's subfolder on a worker thread started from `DllMain`.
- If the host can't load, the proxy keeps forwarding and the game runs without Overglaze.

There is also a variant: the proxy only forwards until the first time Insert is pressed while the game's window is in front, and only then loads the host.

### Late load

Some games refuse any non-system `dxgi.dll` in their folder. For these, nothing goes into the game folder's root, and Overglaze is loaded into the running game:

- either by `overglaze_launch.exe` (set as the Steam launch option), which starts the game unchanged and waits for the first Insert press in the game;
- or by `overglaze_games attach --pid <PID>`.

The injector is deliberately plain: `CreateRemoteThread` calling `LoadLibraryW` with an absolute path, into a process owned by the same user in the same session. There is no manual mapping and no hiding, and the module shows up in the module list. Every check runs before anything is written into the game process, and the loaded DLL checks its installation again on its own.

Loading late has two consequences:

- **Which queue presents is unknown.** DXGI doesn't report the present queue of a D3D12 swap chain. A present-queue witness watches which DIRECT queue runs the command list that moves the current back buffer to the PRESENT state. The panel attaches only after 60 consecutive frames agree, and is withdrawn if the evidence ever contradicts that.
- **Some objects already exist.** Command signatures created before Overglaze arrived have unknown layouts. On the late-load path, `ExecuteIndirect` calls through such signatures are assumed to leave bindings untouched, and each one is counted. Early load is strict and refuses the frame instead.

## Finding the frame: admission

The game manager's check decides the route. Ray Reconstruction is preferred when the game has it; otherwise Super Resolution.

### Streamline route

- **Identity.** Overglaze hooks Streamline's public exported functions, resolved from each module's export table. Modules are identified by their Authenticode signature and their pinned SHA-256. No private offsets are used.
- **Options.** The RR and SR option setters are reached through the public `slGetFeatureFunction`. Overglaze reads the public option structures: `DLSSDOptions` version 3 or later, and the leading members of `DLSSOptions`.
- **Calls observed.** Resource tags (`slSetTag`, `slSetTagForFrame`), constants, and `slEvaluateFeature`.
- **Building the frame.** From the tagged resources: the upscaler's output colour, depth (hardware or linear), and motion vectors, plus the constants (motion-vector scale, jitter, camera). The viewport and the depth type are read from the game's own calls; nothing is guessed from a game name.
- **Short-lived tags.** When a depth or motion-vector tag is declared valid only at tag time (`eOnlyValidNow`), Overglaze copies it into its own texture at the tag call. It does this only when NR will actually use the frame.
- **Native lists.** Command lists wrapped by Streamline are unwrapped through `slGetNativeInterface`. They must be DIRECT lists on the same device as the tagged resources.

### NGX direct route

This route is for games that call NGX themselves, such as Unreal Engine's DLSS plugin; such games may still use Streamline for frame generation or Reflex.

- Overglaze hooks the NGX D3D12 evaluate call.
- Only Ray Reconstruction and Super Resolution evaluations are admitted. Other features, such as frame generation, pass through untouched and are not counted as skips.
- From the NGX parameter block it reads colour input and output, depth, motion vectors, the render sub-rectangle, creation flags (for example whether motion vectors are at render resolution) and the game's exposure texture, pre-exposure and exposure scale.
- Motion vectors at display resolution are resampled onto the depth grid, and their scale is converted.
- Letterboxed or sub-rectangle images are cropped to their valid region. Padding rows are never read or written.

### Checks fail per item

Every check has a name and a counter. When a frame fails a check, Overglaze skips that frame only:

- the game shows the upscaler's own image for that frame;
- NR stays on;
- the next admitted frame asks NR to reset its history.

Checks that prevent harm stay strict: resource states, formats NR has no path for, resource lifetime, threads, and nested calls. They are applied per item, not to the whole call. NR stops only on a real fault, and then Overglaze writes a fault record (`nr-fault.json`) to the session folder.

## Insertion

After the upscaler's evaluate call returns, Overglaze records the following on the same command list, before the game records anything else:

1. **Snapshot** the upscaler's output colour. For targets with a mip chain, only mip 0.
2. **Prepare colour** for the model (see [Colour handoff](#colour-handoff)).
3. **Prepare guides.** Linear view-space depth is converted to hardware depth through the projection. A two-plane depth/stencil resource has its depth plane copied into a single-plane `R32_FLOAT`; NR is never given the two-plane resource. A depth resource with a mip chain is copied at mip 0. Motion vectors are resampled and rescaled when needed.
4. **Evaluate NR** at output resolution, 1:1.
5. **Composite** the result back into the upscaler's output, in place.
6. **Restore the game's bindings** where the game keeps recording without rebinding:
   - root signatures and arguments, descriptor heaps, viewports and scissors, input assembler and stream output, and extended dynamic state;
   - render targets, restored from descriptor contents copied when the game bound them, so a game reusing a descriptor slot is handled.

   The state-block structure is adapted from ReShade's state-tracking example. States it doesn't cover (an active render pass, bundles, unknown indirect layouts on the early path, and others) refuse the insertion instead of guessing.

A private fence marks GPU completion. Overglaze's own resources, and the game resources it uses, stay referenced until that fence completes. If the game resets the command list before submitting it, the frame is discarded and history is reset; NR stays on.

**History.** A break in the frame sequence (menus, loading screens) resets NR's history and keeps NR on. Only a change in resource size rebuilds the NR feature.

## The NR session

The bridge:

- loads the user's model directly, with a restricted DLL search path, and checks the SHA-256 of the file and of the module actually loaded;
- initialises it and creates feature 18 at 1:1 scale;
- sets the `DLSSNR.*` parameters for each frame.

The parameter object records which parameters the model read during each evaluate. That record is the source of the panel's read-back line and of the evidence levels L1–L3. See [MODEL.md](MODEL.md).

## Colour handoff

The model takes display-referred colour, while the upscaler's output is linear floating-point working colour. Overglaze compresses the colour before NR and transfers NR's change back as a per-channel ratio. With **X** the upscaler's linear RGB for a pixel:

**Prepare**

- **H** = M_pre · (E · **X**), where E = 2^stops is the input exposure and M_pre is a 3×3 matrix (identity in the current configuration).
- **S** = T(**H**), where T is a hue-preserving compression. With lo = min(H), hi = max(H), a = lo / (1 + lo) and b = hi / (1 + hi):

  T(**H**) = a + (b − a) · (**H** − lo) / (hi − lo), and T(**H**) = a when hi = lo.

  T maps finite non-negative input into [0, 1) and keeps the ordering of the channels.
- **NR input** = sRGB_encode(saturate(**S**)). **H** and **S** are kept for the composite.

**Composite**

- Optional **edit extrapolation** (off by default, factor n in 1–4): with **C** the NR input and **O** the NR output, **O** is replaced by clamp(**C** + n · (**O** − **C**), 0, 1), per channel. NR runs once and keeps its own output as history; off or n = 1 leaves **O** unchanged.
- **N** = sRGB_decode(NR output)
- **r** = clamp((**N** + ε) / (**S** + ε), 0.01, 10), per channel, with ε = 10⁻⁶
- **result** = M_post · (**H** ⊙ **r**) / E, written back in place. Alpha is kept.

**Properties**

- If the model returns its input unchanged, **r** = 1 and the result is the original **X**, within floating-point precision.
- In dark regions **S** is small, so a small absolute change by the model becomes a large ratio. This is why input exposure matters. If the frame reaches the model too dark, the model sees a nearly black image, and its small edits get amplified into coloured noise.
- Pixels that are not finite, or are negative, are withheld from the model and passed through unchanged.

**Exposure** is Overglaze's own pre-processing; the model never reads it.

- **Manual:** −12 to +10 stops.
- **Automatic:** the game's own exposure first. When the game hands its upscaler an exposure texture (Streamline exposure tag, NGX `ExposureTexture`), Overglaze reads its texel on the GPU and uses E = E_game × `Exposure.Scale` / `Pre.Exposure`, one frame later, as long as the game-exposed log-average stays within 2^−7.19 … 2^−0.42 (with hysteresis, see [CONTROL-PROTOCOL.md](CONTROL-PROTOCOL.md#automatic-exposure)). Otherwise, **metering:** on the GPU, Overglaze measures the log-average luminance of the working colour and computes the gain that maps it to mid-grey (0.18). The gain is smoothed and applied one frame later, within ±14 stops. Before the first reading, it starts at +5 stops. Either way the panel value is an offset on top.

## Control and status

The panel and external clients share one controller with a single writer at a time. The in-game panel holds control by default. See [CONTROL-PROTOCOL.md](CONTROL-PROTOCOL.md).

Status always separates what was **requested** from what was **observed**: the mode actually applied per frame, the parameters the model read back, skipped frames and their reasons, history resets, and GPU completion.

## Data

The data folder is `data\` inside the Overglaze folder (next to `app\`). It is created on first use.

Overglaze keeps its data in its data folder:

- the game library;
- install transactions and recovery copies, kept per game, with the most recent two recovery copies retained;
- panel settings per game;
- one session folder per game launch, holding status, `nr-fault.json` on a fault, and start-up error records.
