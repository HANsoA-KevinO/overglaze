# Third-party source in `runtime/host`

This directory holds the binding-preservation state of the in-game host. One file in it is derived from third-party source; the other is original.

## `binding_state.hpp` (BSD-3-Clause)

- **Upstream:** ReShade v6.8.0 by Patrick Mours, commit `18deaa52de0c425a78b329e9cb3c497281cd00ec`, files `examples/utils/state_tracking.hpp` and `examples/utils/state_tracking.cpp` (Copyright (C) 2022 Patrick Mours).
- **Licence:** upstream offers these two files under BSD-3-Clause or MIT. Overglaze uses them under BSD-3-Clause. The file keeps its `SPDX-License-Identifier: BSD-3-Clause` and the original copyright line. The licence text is in [LICENSE-ReShade.txt](LICENSE-ReShade.txt).
- **What was kept:** the idea of a state block that records the pipeline bindings a command list has set, so they can be put back after inserted work.
- **What changed:**
  - Rewritten for native D3D12 values instead of ReShade's add-on API types.
  - Both root-argument banks (compute and graphics), descriptor heaps, viewports and scissors, render and depth targets, input assembler and stream output, and extended dynamic state.
  - Fixed capacity, explicit defaults, and no partial root constants made up.
  - States it does not cover (an active render pass, bundles, an indirect command of unknown layout, meta commands, work graphs) make the insertion refuse instead of guessing.
- **What it is not:** a resource-state snapshot, a command-stream replay, or a ReShade build. Overglaze does not use ReShade's add-on API, and ReShade's licence does not cover any other part of Overglaze.

## `indirect_layout.hpp` (MIT, original)

Command-signature metadata for `ExecuteIndirect`: when `CreateCommandSignature` succeeds, the public argument layout is stored as private data on that native object. After an `ExecuteIndirect`, only the root parameters and input-assembler bindings the signature touches are reset, following the D3D12 rules for indirect drawing ([Microsoft: Indirect drawing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing)). It reads no indirect argument buffer. A signature with no creation record, a mismatched identity, or an unverified operation is refused.
