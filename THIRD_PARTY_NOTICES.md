# Third-party notices

Overglaze's original code is under the MIT licence (see [LICENSE](LICENSE)). The components below are used under their own licences. Their notices are reproduced here as their licences require, and the full licence texts are in [LICENSES/](LICENSES/).

Dependencies are fetched at pinned versions when building; their source is not copied into this repository unless noted. Binary releases include this file.

| Component | Version | Licence | Used for |
|---|---|---|---|
| Dear ImGui (docking branch) | 1.92.5 | MIT | In-game panel (D3D12 backend), desktop program (D3D11 backend) |
| nlohmann/json | 3.12.0 | MIT | Control protocol, install records, settings |
| MinHook | 1.3.4 | BSD-2-Clause | Function hooks at the D3D12 / DXGI / Streamline / NGX boundaries |
| ReShade (derived source) | 6.8.0 | BSD-3-Clause | State-block structure in `binding_state.hpp` |
| NVIDIA Streamline (public headers, adapted structures) | pinned commit | MIT | Declarations of the public Streamline interface; option-structure layouts in `lab_rr_options.hpp` |
| BakingLab ACES fit | — | MIT | "Soft ACES" viewing curve in the desktop viewer |
| Microsoft Visual C++ v14 x64 runtime (installer and runtime-enabled portable packages) | Packaged file versions are recorded in the release manifest | Microsoft proprietary redistributable | Application-local C++ runtime; not covered by Overglaze's MIT licence |
| Inno Setup | Compiler 6.7.3 | Inno Setup licence | Windows Setup and uninstaller; copyright/website notices remain in the generated installer |

Runtime-enabled packages include only unmodified, signed x64 files from the
Visual Studio `Microsoft.VC143.CRT` redistributable directory. Their Microsoft
copyright and signature remain intact. These files are subject to Microsoft's
terms, not the open-source licences reproduced below. See Microsoft's
[redistribution documentation](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files)
and the [Visual Studio distribution list](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution).

The Simplified Chinese wizard messages in `source/installer/ChineseSimplified.isl`
come from the official [Inno Setup translations](https://jrsoftware.org/files/istrans/)
repository, maintained by Zhenghan Yang (Kira). The translation's original header
is retained. It is an installer translation, not original Overglaze artwork or code.

Not included, not covered by this file:

- **NVIDIA DLSS (NGX) SDK headers.** Proprietary. Whoever builds Overglaze obtains them from NVIDIA and accepts NVIDIA's DLSS SDK licence. They are never committed to this repository. Binaries built with them are subject to NVIDIA's terms for that SDK. No NVIDIA binary is included in Overglaze, and Overglaze is not endorsed by NVIDIA.
- **The NR model (`nvngx_dlssnr.dll`).** Belongs to NVIDIA. Not included or redistributed; the user supplies it. See [docs/MODEL.md](docs/MODEL.md).

---

## Dear ImGui

https://github.com/ocornut/imgui

```
The MIT License (MIT)

Copyright (c) 2014-2025 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Dear ImGui itself bundles:

- `imstb_truetype.h`, `imstb_rectpack.h`, `imstb_textedit.h` — Sean Barrett's stb libraries, available under the MIT licence or as public domain (the user's choice).
- ProggyClean.ttf (embedded default font) — Tristan Grimmer, MIT licence.

At run time the panel and the desktop program load Windows' own system fonts; no font file is redistributed.

## nlohmann/json

https://github.com/nlohmann/json

```
MIT License

Copyright (c) 2013-2025 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

As its own documentation states, the library contains a modified version of the Grisu2 algorithm (Copyright (c) 2009 Florian Loitsch, MIT licence), a copy of Hedley by Evan Nemerson (CC0-1.0, a public-domain dedication that asks for no notice), and parts of Google Abseil (Copyright 2018 The Abseil Authors, Apache-2.0). The single header marks those parts `SPDX-License-Identifier: MIT`; because the library's documentation names Apache-2.0 for the Abseil parts, the Apache License 2.0 text is included in [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt).

## MinHook

https://github.com/TsudaKageyu/minhook

```
MinHook - The Minimalistic API Hooking Library for x64/x86
Copyright (C) 2009-2017 Tsuda Kageyu.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

================================================================================
Portions of this software are Copyright (c) 2008-2009, Vyacheslav Patkov.
================================================================================
Hacker Disassembler Engine 32 C
Copyright (c) 2008-2009, Vyacheslav Patkov.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

-------------------------------------------------------------------------------
Hacker Disassembler Engine 64 C
Copyright (c) 2008-2009, Vyacheslav Patkov.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## ReShade — derived source in `binding_state.hpp`

https://github.com/crosire/reshade

The state-block structure in `binding_state.hpp` is adapted from ReShade v6.8.0, `examples/utils/state_tracking.{hpp,cpp}`, which carries the notice `Copyright (C) 2022 Patrick Mours`. Upstream offers those two files under BSD-3-Clause or MIT; Overglaze uses them under BSD-3-Clause, and the file keeps its BSD-3-Clause identifier and the original copyright line. Overglaze rewrote the structure for native D3D12 (both root-argument banks, descriptor heaps, viewports and scissors, render targets, input assembler and stream output, extended dynamic state), with fixed capacity and explicit refusal of states it does not cover. It does not use ReShade's add-on API and is not a ReShade build. ReShade's licence does not cover any other part of Overglaze, NVIDIA software, or third-party add-ons.

ReShade's licence text:

```
Copyright 2014 Patrick Mours. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
  * Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## NVIDIA Streamline — public headers and adapted structure layouts

https://github.com/NVIDIA-RTX/Streamline

Overglaze compiles against Streamline's public headers (MIT) at a pinned commit, and `lab_rr_options.hpp` contains structure layouts adapted from them:

- the DLSS Ray Reconstruction options structure (`DLSSDOptions`, version 3), adapted from Streamline v2.7.2 `include/sl_dlss_d.h`;
- the leading members of the DLSS Super Resolution options structure (`DLSSOptions`), adapted from `include/sl_dlss.h` at the pinned commit.

The adapted file keeps NVIDIA's notice. The headers carry copyright lines from 2022 to 2024; the notice on the adapted `sl_dlss_d.h` layout reads:

```
Copyright (c) 2023 NVIDIA CORPORATION. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The public Streamline headers used at build time (`sl_core_api.h`, `sl_core_types.h`, `sl_result.h`, and others) carry the same MIT permission notice with `Copyright (c) 2022-2024 NVIDIA CORPORATION. All rights reserved` (the exact years vary by file).

## BakingLab — ACES fit

https://github.com/TheRealMJP/BakingLab (`BakingLab/ACES.hlsl`, Stephen Hill's ACES fit)

Adapted in the desktop viewer's viewing curve (`lab_aces_view.hpp`, `lab_viewer_shader.hpp`).

```
MIT License

Copyright (c) 2016 MJP

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
