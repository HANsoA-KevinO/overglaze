# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
[CmdletBinding()]
param([string]$DependencyRoot = '')
$ErrorActionPreference = 'Stop'
# Default destination: %OVERGLAZE_DEPS% when set, else <checkout>\deps, which is
# where the top-level CMakeLists.txt looks (LAB_DEPS) unless told otherwise.
if (-not $DependencyRoot) { $DependencyRoot = if ($env:OVERGLAZE_DEPS) { $env:OVERGLAZE_DEPS } else { Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'deps' } }
# Public reference headers only, not an installer for Streamline DLLs or drivers.
# The game's 2.7.1 interposer is NOT claimed ABI-verified by this download.
$slCommit = 'fbe73ba0dc817da8db877c62038d415816209139'
$slDestination = Join-Path ([IO.Path]::GetFullPath($DependencyRoot)) ('streamline-headers-' + $slCommit)
if (Test-Path -LiteralPath $slDestination) { throw "Reference directory exists; do not overwrite: $slDestination" }
$slFiles = @('sl_core_api.h','sl_core_types.h','sl_struct.h','sl_consts.h','sl_version.h','sl_result.h','sl_appidentity.h','sl_device_wrappers.h')
$slExpected = @{
    'sl_core_api.h'='7A2A36AE27C09991B0BCCDB28B1A34E24AA688FBEAD21DFAAA41E97513ABF83A'
    'sl_core_types.h'='29D0184A91DC24AEBC0F1B09217DE7B12B6D8186FD8C8C2D021E6E38E0AADF9D'
    'sl_struct.h'='FA28DACBB8BC0574D6DD2403EC8B01B2D811FACD7884251F3834B6E9DBF4D73F'
    'sl_consts.h'='69A9F35AEECD7FBF68A997872BA2E29F27AF7DA4675F76B94628393D14474ABF'
    'sl_version.h'='DC9BF43AE1003D4D94A18E901043724A25DCE73FF64EBC5B1D015DD84EF0E4A2'
    'sl_result.h'='CFD0A3BAD8886A10E311D3F93A52B18E55B721B020A059B00FB0584A4E843C11'
    'sl_appidentity.h'='388D5C18C213E85E87DCC057FB8223B02DEE9572675ED55337A4AFE9B8D93531'
    'sl_device_wrappers.h'='55F71095DA4DA57659752506D120DFB9CCAA360E313CBCE6086A6AC4C1475967'
}
New-Item -ItemType Directory -Path $slDestination | Out-Null
foreach ($slFile in $slFiles) {
    $slUrl = "https://raw.githubusercontent.com/NVIDIAGameWorks/Streamline/$slCommit/include/$slFile"
    $slPath = Join-Path $slDestination $slFile
    Invoke-WebRequest -Uri $slUrl -OutFile $slPath -TimeoutSec 30
    $slItem = Get-Item -LiteralPath $slPath
    if ($slItem.Length -lt 100 -or $slItem.Length -gt 262144) { throw "Unexpected header size: $slFile" }
    $slHash = (Get-FileHash -LiteralPath $slPath -Algorithm SHA256).Hash
    if ($slHash -ne $slExpected[$slFile]) { throw "Header hash changed: $slFile; partial download retained for diagnosis." }
    [pscustomobject]@{File=$slFile;Sha256=$slHash;Source=$slUrl}
}
Write-Output "Public v2.7.2 reference headers: $slDestination. No binary installed or executed; no claim that the 2.7.1 game ABI is verified."
