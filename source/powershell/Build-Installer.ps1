# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
Build an offline Windows installer from a verified, model-free portable payload.
.DESCRIPTION
Requires Inno Setup 6.7 or newer. No dependency downloads, deployment, signing,
game installation, process termination or GPU work is performed. Every output
is new; failed compilation can leave its generated include for inspection.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PayloadDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$CompilerPath,
    [string]$IconPath = '',
    [switch]$ValidateOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IconPath) { $IconPath = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'data/_build/native/controller/overglaze.ico' }
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Security/Microsoft.PowerShell.Security.psd1')

function Assert-NoReparse([string]$Path) {
    $cursor = $Path
    while ($cursor) {
        $item = Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
        if ($null -ne $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Reparse point refused: $cursor"
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
}
function Get-LocalPath([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { throw 'Empty path is not permitted.' }
    $full = [IO.Path]::GetFullPath($Value).TrimEnd('\', '/')
    if ($full -notmatch '^[A-Za-z]:\\.+' -or $full -match '^[A-Za-z]:\\.*:' -or $full.Contains('"') -or $full.Contains("`n") -or $full.Contains("`r")) {
        throw 'Use an ordinary local path below a drive root.'
    }
    Assert-NoReparse $full
    return $full
}
function Is-Within([string]$Path, [string]$Parent) {
    return $Path.Equals($Parent, [StringComparison]::OrdinalIgnoreCase) -or
        $Path.StartsWith($Parent.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)
}
function Assert-New([string]$Path) {
    Assert-NoReparse $Path
    if ($null -ne (Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue)) { throw "Output already exists: $Path" }
}
function Get-Sha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}
function Assert-X64PE([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D) { throw "Not a Windows PE: $Path" }
        $stream.Position = 0x3C
        $offset = $reader.ReadUInt32()
        if ($offset -lt 64 -or $offset -gt ($stream.Length - 24)) { throw "Invalid PE header: $Path" }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw "Not an x64 Windows PE: $Path" }
    } finally { $reader.Dispose(); $stream.Dispose() }
}

$PayloadDirectory = Get-LocalPath $PayloadDirectory
$OutputDirectory = Get-LocalPath $OutputDirectory
if (-not (Test-Path -LiteralPath $PayloadDirectory -PathType Container)) { throw 'Portable payload directory is missing.' }
if ((Is-Within $OutputDirectory $PayloadDirectory) -or (Is-Within $PayloadDirectory $OutputDirectory)) {
    throw 'Installer output and payload must not contain each other.'
}
$manifestPath = Join-Path $PayloadDirectory 'app/release-manifest.json'
Assert-NoReparse $manifestPath
$manifestItem = Get-Item -LiteralPath $manifestPath -Force
if ($manifestItem.PSIsContainer -or $manifestItem.Length -gt 2MB -or $manifestItem.Length -eq 0) { throw 'Invalid release manifest size.' }
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.schema -ne 'overglaze-release-v2' -or $manifest.platform -ne 'windows-x64' -or
    $manifest.model_included -isnot [bool] -or $manifest.model_included -ne $false -or
    $manifest.version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?$' -or
    $manifest.source_commit -notmatch '^[0-9a-f]{40,64}$') { throw 'Invalid model-free Windows release manifest.' }

$runtimeNames = @('concrt140.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
    'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll', 'vccorlib140.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll', 'vcruntime140_threads.dll')
$binaryPaths = @('app/overglaze_viewer.exe', 'app/overglazectl.exe', 'app/overglaze_games.exe',
    'app/overglaze_launch.exe', 'app/plugin/dxgi.dll', 'app/plugin/overglaze_nvngx.dll',
    'app/plugin/overglaze_controller.dll', 'app/tools/overglaze_export_sdr.exe',
    'app/tools/overglaze_capture_stats.exe', 'app/tools/overglaze_check_view_reconstruction.exe',
    'app/tools/overglaze_install_check.exe')
$documentPaths = @('README.md', 'README.zh-CN.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'POLICY.md',
    'SUPPORTED_GAMES.md', 'CHANGELOG.md', 'SECURITY.md', 'CONTRIBUTING.md',
    'LICENSES/MIT.txt', 'LICENSES/BSD-2-Clause.txt', 'LICENSES/BSD-3-Clause.txt', 'LICENSES/Apache-2.0.txt',
    'docs/MODEL.md', 'docs/ADDING-A-GAME.md', 'docs/TROUBLESHOOTING.md', 'docs/ARCHITECTURE.md',
    'docs/CONTROL-PROTOCOL.md', 'docs/PORTABLE.md', 'docs/INSTALLER.md',
    'app/models/README.txt', 'Open-Overglaze.cmd')
$runtimePaths = @($runtimeNames | ForEach-Object { 'app/' + $_ })
$allowed = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($path in @($binaryPaths + $runtimePaths + $documentPaths)) { [void]$allowed.Add($path) }
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
[long]$bytes = 0
foreach ($entry in @($manifest.files)) {
    $relative = [string]$entry.path
    if (-not $allowed.Contains($relative) -or -not $seen.Add($relative) -or
        $entry.sha256 -notmatch '^[0-9a-f]{64}$' -or [long]$entry.size -le 0) { throw "Invalid or unlisted payload entry: $relative" }
    $file = Get-LocalPath (Join-Path $PayloadDirectory $relative)
    if (-not (Is-Within $file $PayloadDirectory)) { throw 'Payload path escaped its root.' }
    $item = Get-Item -LiteralPath $file -Force
    if ($item.PSIsContainer -or $item.Length -ne [long]$entry.size -or (Get-Sha256 $file) -ne $entry.sha256) {
        throw "Payload size/hash mismatch: $relative"
    }
    $bytes += $item.Length
    if ($relative.EndsWith('.exe') -or $relative.EndsWith('.dll')) { Assert-X64PE $file }
}
foreach ($required in @($binaryPaths + $runtimePaths + 'LICENSE' + 'app/models/README.txt' + 'Open-Overglaze.cmd')) {
    if (-not $seen.Contains($required)) { throw "Required installer payload is missing: $required" }
}
if ($bytes -gt 512MB) { throw 'Payload exceeds the 512 MiB installer bound.' }

# Walk one level at a time so a reparse point is rejected before traversing it.
$directories = [Collections.Generic.Queue[string]]::new()
$directories.Enqueue($PayloadDirectory)
while ($directories.Count) {
    $directory = $directories.Dequeue()
    foreach ($item in Get-ChildItem -LiteralPath $directory -Force) {
        Assert-NoReparse $item.FullName
        if ($item.PSIsContainer) { $directories.Enqueue($item.FullName); continue }
        $relative = $item.FullName.Substring($PayloadDirectory.Length + 1).Replace('\', '/')
        if ($relative -ne 'app/release-manifest.json' -and -not $seen.Contains($relative)) { throw "Unlisted file in portable payload: $relative" }
    }
}
$viewer = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $PayloadDirectory 'app/overglaze_viewer.exe'))
if ($viewer.ProductVersion -ne $manifest.version -or $viewer.FileMajorPart -ne [int]($manifest.version.Split('.')[0]) -or
    $viewer.FileMinorPart -ne [int]($manifest.version.Split('.')[1])) {
    throw 'Viewer embedded product version does not match the release manifest.'
}
foreach ($relative in $runtimePaths) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $PayloadDirectory $relative)
    if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') {
        throw "Runtime DLL is not validly Microsoft-signed: $relative"
    }
}
if ($ValidateOnly) {
    [pscustomobject]@{ validated = $true; version = $manifest.version; files = $seen.Count; bytes = $bytes; model_included = $false }
    return
}

$IconPath = Get-LocalPath $IconPath
if (-not (Test-Path -LiteralPath $IconPath -PathType Leaf)) { throw 'Generated Overglaze icon is missing; pass -IconPath.' }
if ([string]::IsNullOrWhiteSpace($CompilerPath)) {
    $CompilerPath = $env:OVERGLAZE_ISCC
    if ([string]::IsNullOrWhiteSpace($CompilerPath)) {
        $command = Get-Command 'ISCC.exe' -ErrorAction SilentlyContinue
        if ($null -ne $command) { $CompilerPath = $command.Source }
    }
}
$CompilerPath = Get-LocalPath $CompilerPath
if (-not (Test-Path -LiteralPath $CompilerPath -PathType Leaf)) { throw 'ISCC.exe is missing; pass -CompilerPath.' }
$compilerVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($CompilerPath)
# Official ISCC builds may carry 0.0.0.0 PE metadata. The .iss enforces the
# actual engine version through ISPP Ver; do not reject a valid compiler based
# on absent Windows version resources.
$scriptPath = Get-LocalPath (Join-Path (Split-Path $PSScriptRoot -Parent) 'installer/Overglaze.iss')
$name = 'Overglaze-Setup-' + $manifest.version + '-win64'
$installer = Join-Path $OutputDirectory ($name + '.exe')
$checksum = $installer + '.sha256'
$receipt = $installer + '.receipt.json'
$include = Join-Path $OutputDirectory ($name + '.files.iss')
foreach ($path in @($installer, $checksum, $receipt, $include)) { Assert-New $path }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Assert-NoReparse $OutputDirectory
$lines = [Collections.Generic.List[string]]::new()
# The temporary helper is extracted with its app-local runtime before app files
# are written. It never relies on a prerequisite download or a system install.
foreach ($relative in @('app/overglaze_games.exe') + $runtimePaths) {
    $entry = @($manifest.files | Where-Object { $_.path -eq $relative })[0]
    $lines.Add('Source: "' + (Join-Path $PayloadDirectory $relative) + '"; Flags: dontcopy noencryption; Hash: "' + $entry.sha256 + '"')
}
foreach ($entry in @($manifest.files | Sort-Object path)) {
    $parent = [IO.Path]::GetDirectoryName($entry.path.Replace('/', '\'))
    $destination = '{app}'
    if ($parent) { $destination += '\' + $parent }
    $lines.Add('Source: "' + (Join-Path $PayloadDirectory $entry.path) + '"; DestDir: "' + $destination + '"; Flags: ignoreversion; Hash: "' + $entry.sha256 + '"')
}
$lines.Add('Source: "' + $manifestPath + '"; DestDir: "{app}\app"; Flags: ignoreversion; Hash: "' + (Get-Sha256 $manifestPath) + '"')
[IO.File]::WriteAllLines($include, $lines, [Text.UTF8Encoding]::new($true))
& $CompilerPath '/Qp' (('/O' + $OutputDirectory)) ('/DPayloadDirectory=' + $PayloadDirectory) ('/DPayloadFiles=' + $include) `
    ('/DPackageVersion=' + $manifest.version) ('/DIconPath=' + $IconPath) $scriptPath
if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed ($LASTEXITCODE)." }
if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) { throw 'Inno Setup did not emit the expected installer.' }
$sha = Get-Sha256 $installer
[IO.File]::WriteAllText($checksum, "$sha  $name.exe`n", [Text.UTF8Encoding]::new($false))
$record = [ordered]@{ schema = 'overglaze-installer-build-v1'; version = $manifest.version;
    app_id = '{66B920DD-3026-4D7D-AC73-64E349940B95}'; platform = 'windows-x64';
    source_commit = $manifest.source_commit; source_dirty = $manifest.source_dirty;
    release_manifest_sha256 = (Get-Sha256 $manifestPath); installer_sha256 = $sha;
    compiler_version = $(if ($compilerVersion.FileMajorPart) { $compilerVersion.FileVersion } else { 'PE version unavailable; Inno 6.7+ in 6.x checked by ISPP Ver' });
    compiler_sha256 = (Get-Sha256 $CompilerPath); payload_files = $seen.Count;
    payload_bytes = $bytes; model_included = $false; signed = $false }
[IO.File]::WriteAllText($receipt, (($record | ConvertTo-Json -Depth 5) + "`n"), [Text.UTF8Encoding]::new($false))
[pscustomobject]@{ installer = $installer; sha256 = $sha; receipt = $receipt; version = $manifest.version }
