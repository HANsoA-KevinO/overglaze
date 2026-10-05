# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [string]$Destination = '',
    # The NGX (DLSS) SDK headers are NVIDIA's, under the NVIDIA DLSS SDK licence
    # (https://github.com/NVIDIA/DLSS). Downloading them means accepting it, so a
    # fresh download needs this switch; verifying an existing copy does not.
    [switch]$AcceptNvidiaDlssSdkLicense
)
$ErrorActionPreference = 'Stop'
if (-not $Destination) { $deps = if ($env:OVERGLAZE_DEPS) { $env:OVERGLAZE_DEPS } else { Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'deps' }; $Destination = Join-Path $deps 'ngx-headers' }
$expectedCommit = 'a291cc7d2cc642a51566f3dfd5376f635cd1b284'
if (-not (Test-Path -LiteralPath $Destination)) {
    if (-not $AcceptNvidiaDlssSdkLicense) { throw 'The NGX SDK headers are under the NVIDIA DLSS SDK licence: read it at https://github.com/NVIDIA/DLSS, then re-run with -AcceptNvidiaDlssSdkLicense' }
    & git clone --depth 1 --filter=blob:none --sparse --no-checkout https://github.com/NVIDIA/DLSS.git $Destination
    if ($LASTEXITCODE -ne 0) { throw 'NGX header clone failed' }
    $actualCommit = & git -C $Destination rev-parse HEAD
    if ($actualCommit -ne $expectedCommit) { throw "Upstream moved; inspect before using: $actualCommit" }
    & git -C $Destination sparse-checkout set include
    if ($LASTEXITCODE -ne 0) { throw 'Sparse include selection failed' }
    & git -C $Destination switch --detach $expectedCommit
    if ($LASTEXITCODE -ne 0) { throw 'Header checkout failed' }
}
$actualCommit = & git -C $Destination rev-parse HEAD
if ($actualCommit -ne $expectedCommit) { throw "Unexpected NGX header commit: $actualCommit" }
if (-not (Test-Path -LiteralPath (Join-Path $Destination 'include\nvsdk_ngx.h'))) { throw 'Missing header payload' }
Write-Output "NGX headers verified: $actualCommit (local build dependency; NVIDIA license retained)"
