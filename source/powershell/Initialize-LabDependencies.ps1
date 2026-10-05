# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [string]$Destination = '',
    # ReShade v6.8.0 is a research-track dependency only; fetched with -Research,
    # or automatically in a tree that has native\research.
    [switch]$Research
)
$ErrorActionPreference = 'Stop'
# Default destination: %OVERGLAZE_DEPS% when set, else <checkout>\deps, which is
# where the top-level CMakeLists.txt looks (LAB_DEPS) unless told otherwise.
if (-not $Destination) { $Destination = if ($env:OVERGLAZE_DEPS) { $env:OVERGLAZE_DEPS } else { Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'deps' } }
$withResearch = $Research -or (Test-Path -LiteralPath (Join-Path (Split-Path $PSScriptRoot -Parent) 'native\research'))
$dependencies = @(
    @{ Name='imgui-1.92.5-docking'; Repository='https://github.com/ocornut/imgui.git'; Tag='v1.92.5-docking'; Commit='3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c' },
    @{ Name='json'; Repository='https://github.com/nlohmann/json.git'; Tag='v3.12.0'; Commit='55f93686c01528224f448c19128836e7df245f72' },
    @{ Name='reshade'; Repository='https://github.com/crosire/reshade.git'; Tag='v6.8.0'; Commit='18deaa52de0c425a78b329e9cb3c497281cd00ec'; Research=$true },
    @{ Name='minhook'; Repository='https://github.com/TsudaKageyu/minhook.git'; Tag='v1.3.4'; Commit='c3fcafdc10146beb5919319d0683e44e3c30d537' }
)
foreach ($dependency in $dependencies) {
    if ($dependency.Research -and -not $withResearch) { continue }
    $target = Join-Path $Destination $dependency.Name
    if (-not (Test-Path -LiteralPath $target)) {
        New-Item -ItemType Directory -Path $Destination -Force | Out-Null
        & git clone --depth 1 --branch $dependency.Tag -- $dependency.Repository $target
        if ($LASTEXITCODE -ne 0) { throw "Clone failed; preserved partial directory: $target" }
    }
    $commit = & git -C $target rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $commit -ne $dependency.Commit) { throw "Unexpected dependency commit: $target ($commit)" }
    $changes = & git -C $target status --porcelain --untracked-files=normal
    if ($LASTEXITCODE -ne 0 -or $changes) { throw "Dependency has local changes: $target" }
    Write-Output "$($dependency.Name): $commit verified"
}
