# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
Assemble a model-free, allowlisted Windows portable preview without publishing it.
.DESCRIPTION
Existing output is never overwritten or deleted. Inputs and their ancestors must
be ordinary local paths. The manifest describes the checkout at packaging time;
it does not claim that arbitrary supplied binaries were built from that commit.
#>
[CmdletBinding()]
param(
    [string]$RepositoryRoot = '',
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    # Optional application-local x64 Microsoft.VC143.CRT redistributable directory.
    # Only the named, signed Microsoft runtime files below can enter the payload.
    [string]$RuntimeDirectory = '',
    [ValidatePattern('^\d+\.\d+\.\d+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?$')]
    [string]$Version = '0.2.0-preview.2'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $RepositoryRoot) { $RepositoryRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent }

function Get-LocalPath([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { throw 'Empty path is not permitted.' }
    $full = [IO.Path]::GetFullPath($Value).TrimEnd('\', '/')
    if ($full -notmatch '^[A-Za-z]:\\.+' -or $full -match '^[A-Za-z]:\\.*:') {
        throw 'Use a local directory below a drive root; network, device and alternate-stream paths are refused.'
    }
    Assert-NoReparse $full
    return $full
}

function Assert-NoReparse([string]$Path) {
    $cursor = $Path
    while ($cursor) {
        # Get-Item also sees a broken link; Test-Path alone can miss it.
        $item = Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
        if ($null -ne $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Reparse point refused: $cursor"
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
}

function Is-Within([string]$Path, [string]$Parent) {
    return $Path.Equals($Parent, [StringComparison]::OrdinalIgnoreCase) -or
        $Path.StartsWith($Parent.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)
}

function Assert-New([string]$Path) {
    Assert-NoReparse $Path
    if ($null -ne (Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue)) {
        throw "Output already exists; choose another output directory: $Path"
    }
}

function Write-Utf8([string]$Path, [string]$Text) {
    Assert-New $Path
    [IO.File]::WriteAllText($Path, $Text.Replace("`r`n", "`n"), [Text.UTF8Encoding]::new($false))
}

function Get-Sha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}

$RepositoryRoot = Get-LocalPath $RepositoryRoot
$BuildDirectory = Get-LocalPath $BuildDirectory
$OutputDirectory = Get-LocalPath $OutputDirectory
foreach ($dir in @($RepositoryRoot, $BuildDirectory)) {
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) { throw "Missing input directory: $dir" }
}
if ((Is-Within $RepositoryRoot $OutputDirectory) -or (Is-Within $OutputDirectory $BuildDirectory)) {
    throw 'Output must not contain the checkout or be inside the build directory.'
}
$name = "Overglaze-$Version-win64"
$stage = Join-Path $OutputDirectory $name
$archive = Join-Path $OutputDirectory "$name.zip"
$checksum = Join-Path $OutputDirectory "$name.zip.sha256"
foreach ($path in @($stage, $archive, $checksum)) {
    if (-not (Is-Within $path $OutputDirectory)) { throw 'Output escaped its declared directory.' }
    Assert-New $path
}

# Explicit paths only: no directory recursion, globs, model files, build-root
# override, PDBs, logs, screenshots, game facts or generated adapter packages.
$binaryMap = [ordered]@{
    'overglaze_viewer.exe' = 'app/overglaze_viewer.exe'
    'overglazectl.exe' = 'app/overglazectl.exe'
    'overglaze_games.exe' = 'app/overglaze_games.exe'
    'overglaze_launch.exe' = 'app/overglaze_launch.exe'
    'controller/dxgi.dll' = 'app/plugin/dxgi.dll'
    'controller/overglaze_nvngx.dll' = 'app/plugin/overglaze_nvngx.dll'
    'controller/overglaze_controller.dll' = 'app/plugin/overglaze_controller.dll'
    'overglaze_export_sdr.exe' = 'app/tools/overglaze_export_sdr.exe'
    'overglaze_capture_stats.exe' = 'app/tools/overglaze_capture_stats.exe'
    'overglaze_check_view_reconstruction.exe' = 'app/tools/overglaze_check_view_reconstruction.exe'
    'lab_installation_tests.exe' = 'app/tools/overglaze_install_check.exe'
}
$documents = @(
    'README.md', 'README.zh-CN.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'POLICY.md',
    'SUPPORTED_GAMES.md', 'CHANGELOG.md', 'SECURITY.md', 'CONTRIBUTING.md',
    'LICENSES/MIT.txt', 'LICENSES/BSD-2-Clause.txt', 'LICENSES/BSD-3-Clause.txt', 'LICENSES/Apache-2.0.txt',
    'docs/MODEL.md', 'docs/ADDING-A-GAME.md', 'docs/TROUBLESHOOTING.md',
    'docs/ARCHITECTURE.md', 'docs/CONTROL-PROTOCOL.md', 'docs/PORTABLE.md', 'docs/INSTALLER.md'
)
$inputs = @()
foreach ($entry in $binaryMap.GetEnumerator()) {
    $inputs += [pscustomobject]@{ Source = (Join-Path $BuildDirectory $entry.Key); Destination = $entry.Value }
}
foreach ($document in $documents) {
    $inputs += [pscustomobject]@{ Source = (Join-Path $RepositoryRoot $document); Destination = $document }
}
$runtimeNames = @(
    'concrt140.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
    'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll', 'vccorlib140.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll', 'vcruntime140_threads.dll'
)
if ($RuntimeDirectory) {
    # Use this host's security module even if a PowerShell 7 parent passed a
    # PSModulePath to a Windows PowerShell 5 child (or vice versa).
    Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Security/Microsoft.PowerShell.Security.psd1') -ErrorAction Stop
    $RuntimeDirectory = Get-LocalPath $RuntimeDirectory
    if (-not (Test-Path -LiteralPath $RuntimeDirectory -PathType Container)) { throw 'Missing Microsoft runtime directory.' }
    foreach ($runtimeName in $runtimeNames) {
        $runtimeFile = Join-Path $RuntimeDirectory $runtimeName
        Assert-NoReparse $runtimeFile
        $runtimeSignature = Get-AuthenticodeSignature -LiteralPath $runtimeFile
        if ($runtimeSignature.Status -ne 'Valid' -or $null -eq $runtimeSignature.SignerCertificate -or
            $runtimeSignature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation(?:,|$)') {
            throw "Microsoft runtime signature refused: $runtimeName"
        }
        $runtimeInfo = (Get-Item -LiteralPath $runtimeFile).VersionInfo
        if ($runtimeInfo.FileMajorPart -ne 14) { throw "Unexpected Microsoft runtime version: $runtimeName" }
        $runtimeStream = [IO.File]::OpenRead($runtimeFile)
        $runtimeReader = [IO.BinaryReader]::new($runtimeStream)
        try {
            if ($runtimeStream.Length -lt 64 -or $runtimeReader.ReadUInt16() -ne 0x5A4D) { throw 'Runtime is not a PE image.' }
            $runtimeStream.Position = 0x3C
            $runtimePeOffset = $runtimeReader.ReadUInt32()
            if ($runtimePeOffset -gt $runtimeStream.Length - 6) { throw 'Invalid runtime PE header.' }
            $runtimeStream.Position = $runtimePeOffset
            if ($runtimeReader.ReadUInt32() -ne 0x00004550 -or $runtimeReader.ReadUInt16() -ne 0x8664) {
                throw "Microsoft runtime must be x64: $runtimeName"
            }
        } finally { $runtimeReader.Dispose() }
        $inputs += [pscustomobject]@{ Source = $runtimeFile; Destination = "app/$runtimeName" }
    }
}
[long]$bytes = 0
foreach ($inputFile in $inputs) {
    Assert-NoReparse $inputFile.Source
    $item = Get-Item -LiteralPath $inputFile.Source -Force
    if ($item.PSIsContainer -or $item.Length -eq 0) { throw "Missing or empty input file: $($inputFile.Source)" }
    $bytes += $item.Length
}
if ($bytes -gt 512MB) { throw 'Allowlisted inputs exceed the 512 MiB packaging bound.' }

# No username, local paths, diff text, remote URL, or untracked filenames enter
# the manifest. Dirty is an observed fact, never inferred from a release name.
$commitOutput = & git -C $RepositoryRoot rev-parse --verify HEAD
if ($LASTEXITCODE -ne 0) { throw 'Repository must have a Git HEAD for source provenance.' }
$sourceCommit = ($commitOutput -join '').Trim()
if ($sourceCommit -notmatch '^[0-9a-f]{40,64}$') { throw 'Unexpected Git commit identity.' }
$gitRootOutput = & git -C $RepositoryRoot rev-parse --show-toplevel
if ($LASTEXITCODE -ne 0 -or (Get-LocalPath ($gitRootOutput -join '')) -ne $RepositoryRoot) {
    throw 'RepositoryRoot must be the exact Git checkout root.'
}
$statusOutput = @(& git -C $RepositoryRoot status --porcelain=v1 --untracked-files=normal)
if ($LASTEXITCODE -ne 0) { throw 'Cannot determine source checkout state.' }
$sourceDirty = $statusOutput.Count -gt 0

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Assert-NoReparse $OutputDirectory
Assert-New $stage
New-Item -ItemType Directory -Path $stage | Out-Null
foreach ($inputFile in $inputs) {
    $destination = Join-Path $stage $inputFile.Destination
    if (-not (Is-Within $destination $stage)) { throw 'Allowlist destination escaped the package.' }
    Assert-NoReparse $inputFile.Source
    Assert-NoReparse $destination
    New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
    Assert-New $destination
    Copy-Item -LiteralPath $inputFile.Source -Destination $destination
}
New-Item -ItemType Directory -Path (Join-Path $stage 'app/models') | Out-Null
Write-Utf8 (Join-Path $stage 'app/models/README.txt') @'
Place your own, unmodified nvngx_dlssnr.dll in this folder.
Overglaze does not include, download or redistribute the NVIDIA model.
See ../../docs/MODEL.md for the accepted model identity.
'@
Write-Utf8 (Join-Path $stage 'Open-Overglaze.cmd') @'
@echo off
rem SPDX-FileCopyrightText: 2026 HANsoA-KevinO
rem SPDX-License-Identifier: MIT
setlocal
if not exist "%~dp0app\overglaze_viewer.exe" (
  echo Overglaze is incomplete. Extract the entire ZIP before opening it.
  pause
  exit /b 1
)
start "" "%~dp0app\overglaze_viewer.exe"
'@

$paths = [string[]]@(Get-ChildItem -LiteralPath $stage -File -Recurse | ForEach-Object {
    $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
})
[Array]::Sort($paths, [StringComparer]::Ordinal)
$files = @($paths | ForEach-Object {
    $file = Join-Path $stage $_
    [ordered]@{ path = $_; size = (Get-Item -LiteralPath $file).Length;
        sha256 = (Get-Sha256 $file) }
})
$manifest = [ordered]@{
    schema = 'overglaze-release-v2'
    version = $Version
    channel = 'preview'
    platform = 'windows-x64'
    source_commit = $sourceCommit
    source_dirty = $sourceDirty
    provenance_scope = 'Checkout observed at packaging time; supplied binary provenance is not independently verified.'
    model_included = $false
    runtime_included = [bool]$RuntimeDirectory
    runtime_files = @(if ($RuntimeDirectory) { $runtimeNames })
    files = $files
}
Write-Utf8 (Join-Path $stage 'app/release-manifest.json') (($manifest | ConvertTo-Json -Depth 6) + "`n")

# Stable entry ordering and fixed metadata make packaging repeatable for the
# same bytes, checkout state and .NET compression implementation.
Add-Type -AssemblyName System.IO.Compression
$stream = [IO.File]::Open($archive, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        $archivePaths = [string[]]@($paths + 'app/release-manifest.json')
        [Array]::Sort($archivePaths, [StringComparer]::Ordinal)
        foreach ($relative in $archivePaths) {
            $entry = $zip.CreateEntry("$name/$relative", [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = [DateTimeOffset]::new(2000, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
            $entryStream = $entry.Open()
            try {
                $inputStream = [IO.File]::OpenRead((Join-Path $stage $relative))
                try { $inputStream.CopyTo($entryStream) } finally { $inputStream.Dispose() }
            } finally { $entryStream.Dispose() }
        }
    } finally { $zip.Dispose() }
} finally { $stream.Dispose() }
$archiveSha = Get-Sha256 $archive
Write-Utf8 $checksum "$archiveSha  $name.zip`n"
[pscustomobject]@{ archive = $archive; sha256 = $archiveSha; stage = $stage;
    source_commit = $sourceCommit; source_dirty = $sourceDirty; version = $Version }
