# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    # A self-contained toolchain folder (Python312, CMake\cmake-4.4.3-windows-x86_64,
    # Ninja, VSBuildTools2022). Default: %OVERGLAZE_TOOLCHAIN_ROOT%, else the value an
    # optional machine-local Enter-Environment.local.ps1 beside this script names,
    # else none: Visual Studio is found with vswhere and CMake, Ninja and Python
    # come from PATH (Visual Studio's own CMake and Ninja count).
    [string]$ToolchainRoot = ''
)

$ErrorActionPreference = 'Stop'

# Machine-local defaults: may set $localToolchainRoot and OVERGLAZE_* variables.
$localToolchainRoot = ''
$localDefaults = Join-Path $PSScriptRoot 'Enter-Environment.local.ps1'
if (Test-Path -LiteralPath $localDefaults) { . $localDefaults }
if (-not $ToolchainRoot) { $ToolchainRoot = if ($env:OVERGLAZE_TOOLCHAIN_ROOT) { $env:OVERGLAZE_TOOLCHAIN_ROOT } else { $localToolchainRoot } }

$bins = @()
if ($ToolchainRoot) {
    $required = [ordered]@{
        PythonBin = Join-Path $ToolchainRoot 'Python312'
        CMakeBin  = Join-Path $ToolchainRoot 'CMake\cmake-4.4.3-windows-x86_64\bin'
        NinjaBin  = Join-Path $ToolchainRoot 'Ninja'
        VsDevCmd  = Join-Path $ToolchainRoot 'VSBuildTools2022\Common7\Tools\VsDevCmd.bat'
    }
    foreach ($entry in $required.GetEnumerator()) {
        if (-not (Test-Path -LiteralPath $entry.Value)) { throw "Missing $($entry.Key): $($entry.Value)" }
    }
    $vsDevCmd = $required.VsDevCmd
    $bins += $required.PythonBin, $required.CMakeBin, $required.NinjaBin
    # Optional research tools: on PATH when the toolchain folder has them.
    foreach ($optional in @('PresentMon', 'NsightGraphics2026.2\host\windows-desktop-nomad-x64')) {
        $p = Join-Path $ToolchainRoot $optional
        if (Test-Path -LiteralPath $p) { $bins += $p }
    }
    $deps = Join-Path $ToolchainRoot 'dlsslab-deps'
    if (-not $env:OVERGLAZE_DEPS -and (Test-Path -LiteralPath $deps)) { $env:OVERGLAZE_DEPS = $deps }
    $env:OVERGLAZE_TOOLCHAIN_ROOT = $ToolchainRoot
} else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw "Visual Studio 2022 with the C++ workload is required (vswhere.exe not found: $vswhere)" }
    $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $vsRoot) { throw 'No Visual Studio installation with the MSVC x64 tools was found' }
    $vsDevCmd = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $vsDevCmd)) { throw "Missing VsDevCmd.bat: $vsDevCmd" }
}

# Import the x64 MSVC/Windows SDK environment into this PowerShell process.
$vsCommand = "`"$vsDevCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
$importedNames = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase
)
& $env:ComSpec /d /s /c $vsCommand | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        $name = $Matches[1]
        # Some hosts expose both PATH and Path. `cmd set` places the developer
        # PATH first; keep that value instead of letting the later casing alias
        # overwrite it with the pre-VsDevCmd path.
        if ($importedNames.Add($name)) {
            [Environment]::SetEnvironmentVariable($name, $Matches[2], 'Process')
        }
    }
}
if ($LASTEXITCODE -ne 0) {
    throw "VsDevCmd failed with exit code $LASTEXITCODE"
}

$pathEntries = @($bins + ($env:Path -split ';')) |
    Where-Object { $_ } |
    Select-Object -Unique
$env:Path = $pathEntries -join ';'
$env:PYTHONIOENCODING = 'utf-8'

function Find-Tool([string[]]$names, [bool]$required) {
    foreach ($n in $names) { $c = Get-Command $n -ErrorAction SilentlyContinue | Select-Object -First 1; if ($c) { return $c.Source } }
    if ($required) { throw "$($names[0]) not found on PATH" }
    return $null
}
$python = Find-Tool @('python.exe', 'python3.exe') $true
if (-not $env:OVERGLAZE_PYTHON) { $env:OVERGLAZE_PYTHON = $python }

[pscustomobject]@{
    ToolchainRoot = if ($ToolchainRoot) { $ToolchainRoot } else { '(none: Visual Studio and PATH)' }
    Python        = $python
    CMake         = Find-Tool @('cmake.exe') $true
    Ninja         = Find-Tool @('ninja.exe') $true
    Compiler      = Find-Tool @('cl.exe') $true
    Linker        = Find-Tool @('link.exe') $true
    Dependencies  = $env:OVERGLAZE_DEPS
    PresentMon    = Find-Tool @('presentmon.exe') $false
    Nsight        = Find-Tool @('ngfx.exe') $false
}
