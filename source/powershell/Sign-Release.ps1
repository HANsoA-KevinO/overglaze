# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
Write and sign the self-update manifest of one release.
.DESCRIPTION
Computes the size and SHA-256 of the Setup and the portable ZIP, writes
overglaze-update.json (schema overglaze-update-v1, UTF-8 without BOM; these
exact bytes are signed) and overglaze-update.json.sig (base64 of the 64-byte
ECDSA P-256 / SHA-256 signature r||s, one line) next to the Setup, and verifies
the signature with the release public key before it finishes.

The private key is a CNG EccPrivateBlob kept outside every git work tree:
-KeyPath, or the environment variable OVERGLAZE_RELEASE_KEY. The script refuses
a key inside a git work tree, never prints or copies it, and writes nothing
when the signature does not verify with -PublicKeyPath (default: the
repository's assets/release-key/overglaze-release-p256.public.blob).

Attach both files to the GitHub release next to the Setup and the ZIP. Nothing
is uploaded or published by this script.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SetupPath,
    [Parameter(Mandatory = $true)][string]$ZipPath,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?$')]
    [string]$Version,
    [Parameter(Mandatory = $true)][ValidateSet('preview', 'stable')][string]$Channel,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-f]{40,64}$')][string]$SourceCommit,
    [Parameter(Mandatory = $true)][string]$NotesFile,
    [string]$KeyPath = '',
    [string]$PublicKeyPath = '',
    # The release date (yyyy-MM-dd); default: today in UTC.
    [string]$Published = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repository = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent

function Assert-NoReparse([string]$Path) {
    $cursor = $Path
    while ($cursor) {
        $item = Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
        if ($null -ne $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Reparse point refused: $cursor" }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
}
function Get-LocalFile([string]$Value, [string]$What) {
    if ([string]::IsNullOrWhiteSpace($Value)) { throw "$What is required." }
    $full = [IO.Path]::GetFullPath($Value)
    if ($full -notmatch '^[A-Za-z]:\\.+' -or $full -match '^[A-Za-z]:\\.*:') { throw "$What must be an ordinary local path." }
    Assert-NoReparse $full
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { throw "$What is missing: $full" }
    return $full
}
# A .git directory (a repository) or file (a worktree or submodule) in the
# key's folder or any folder above it.
function Get-GitWorkTree([string]$Path) {
    $cursor = [IO.Path]::GetDirectoryName($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath (Join-Path $cursor '.git')) { return $cursor }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if (-not $parent -or $parent -eq $cursor) { break }
        $cursor = $parent
    }
    return $null
}
function Get-Sha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}
function ConvertTo-JsonString([string]$Value) {
    $builder = [Text.StringBuilder]::new()
    [void]$builder.Append('"')
    foreach ($ch in $Value.ToCharArray()) {
        $code = [int]$ch
        if ($code -eq 0x22) { [void]$builder.Append('\"') }
        elseif ($code -eq 0x5C) { [void]$builder.Append('\\') }
        elseif ($code -eq 0x0A) { [void]$builder.Append('\n') }
        elseif ($code -eq 0x09) { [void]$builder.Append('\t') }
        elseif ($code -lt 0x20) { [void]$builder.Append(('\u{0:x4}' -f $code)) }
        else { [void]$builder.Append($ch) }
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}
function Get-FileEntry([string]$Path, [string]$Name) {
    $size = (Get-Item -LiteralPath $Path -Force).Length
    if ($size -le 0 -or $size -gt 1GB) { throw "File size outside 1 byte .. 1 GiB: $Name" }
    return [ordered]@{ name = $Name; size = [long]$size; sha256 = (Get-Sha256 $Path) }
}
function Test-Signature([byte[]]$Data, [byte[]]$Signature, [byte[]]$PublicBlob) {
    $key = [Security.Cryptography.CngKey]::Import($PublicBlob, [Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
    try {
        $verifier = [Security.Cryptography.ECDsaCng]::new($key)
        try {
            $verifier.HashAlgorithm = [Security.Cryptography.CngAlgorithm]::Sha256
            return $verifier.VerifyData($Data, $Signature)
        } finally { $verifier.Dispose() }
    } finally { $key.Dispose() }
}

# ---- inputs
$SetupPath = Get-LocalFile $SetupPath 'SetupPath'
$ZipPath = Get-LocalFile $ZipPath 'ZipPath'
$NotesFile = Get-LocalFile $NotesFile 'NotesFile'
if (-not $KeyPath) { $KeyPath = $env:OVERGLAZE_RELEASE_KEY }
if ([string]::IsNullOrWhiteSpace($KeyPath)) { throw 'Pass -KeyPath or set OVERGLAZE_RELEASE_KEY to the release private key (EccPrivateBlob).' }
$KeyPath = Get-LocalFile $KeyPath 'KeyPath'
$workTree = Get-GitWorkTree $KeyPath
if ($workTree) { throw "Refused: the private key is inside a git work tree ($workTree). Keep it outside every repository." }
if (-not $PublicKeyPath) { $PublicKeyPath = Join-Path $repository 'assets\release-key\overglaze-release-p256.public.blob' }
$PublicKeyPath = Get-LocalFile $PublicKeyPath 'PublicKeyPath'
if (-not $Published) { $Published = [DateTime]::UtcNow.ToString('yyyy-MM-dd', [Globalization.CultureInfo]::InvariantCulture) }
if ($Published -notmatch '^\d{4}-\d{2}-\d{2}$') { throw 'Published must be yyyy-MM-dd.' }

$setupName = [IO.Path]::GetFileName($SetupPath)
$zipName = [IO.Path]::GetFileName($ZipPath)
if ($setupName -cne "Overglaze-Setup-$Version-win64.exe") { throw "SetupPath must be Overglaze-Setup-$Version-win64.exe (got $setupName)." }
if ($zipName -cne "Overglaze-$Version-win64.zip") { throw "ZipPath must be Overglaze-$Version-win64.zip (got $zipName)." }

$outputDirectory = [IO.Path]::GetDirectoryName($SetupPath)
$manifestPath = Join-Path $outputDirectory 'overglaze-update.json'
$signaturePath = Join-Path $outputDirectory 'overglaze-update.json.sig'
foreach ($path in @($manifestPath, $signaturePath)) {
    if ($null -ne (Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue)) { throw "Output already exists: $path" }
}

$publicBlob = [IO.File]::ReadAllBytes($PublicKeyPath)
if ($publicBlob.Length -ne 72 -or [Text.Encoding]::ASCII.GetString($publicBlob, 0, 4) -cne 'ECS1') {
    throw 'PublicKeyPath is not a P-256 BCRYPT_ECCPUBLIC_BLOB (72 bytes, ECS1).'
}

$notes = [IO.File]::ReadAllText($NotesFile, [Text.UTF8Encoding]::new($false, $true))
$notes = $notes.TrimStart([char]0xFEFF).Replace("`r`n", "`n").Replace("`r", "`n").Trim()
if (-not $notes) { throw 'NotesFile is empty.' }
if ([Text.Encoding]::UTF8.GetByteCount($notes) -gt 16KB) { throw 'NotesFile exceeds 16 KiB.' }

# ---- manifest: these exact bytes are what is signed and published
$setup = Get-FileEntry $SetupPath $setupName
$portable = Get-FileEntry $ZipPath $zipName
function Format-Entry($Entry) {
    return "{`n    ""name"": " + (ConvertTo-JsonString $Entry.name) + ",`n    ""size"": " + $Entry.size.ToString([Globalization.CultureInfo]::InvariantCulture) +
        ",`n    ""sha256"": " + (ConvertTo-JsonString $Entry.sha256) + "`n  }"
}
$text = "{`n" +
    "  ""schema"": ""overglaze-update-v1"",`n" +
    "  ""version"": " + (ConvertTo-JsonString $Version) + ",`n" +
    "  ""channel"": " + (ConvertTo-JsonString $Channel) + ",`n" +
    "  ""published"": " + (ConvertTo-JsonString $Published) + ",`n" +
    "  ""source_commit"": " + (ConvertTo-JsonString $SourceCommit) + ",`n" +
    "  ""notes_zh"": " + (ConvertTo-JsonString $notes) + ",`n" +
    "  ""setup"": " + (Format-Entry $setup) + ",`n" +
    "  ""portable"": " + (Format-Entry $portable) + "`n" +
    "}`n"
$manifestBytes = [Text.UTF8Encoding]::new($false).GetBytes($text)
if ($manifestBytes.Length -gt 60KB) { throw 'Manifest exceeds the 64 KiB the program accepts.' }

# ---- sign; the key bytes never leave this block
$keyBytes = [IO.File]::ReadAllBytes($KeyPath)
try {
    $key = [Security.Cryptography.CngKey]::Import($keyBytes, [Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
} finally { [Array]::Clear($keyBytes, 0, $keyBytes.Length) }
try {
    if ($key.Algorithm -ne [Security.Cryptography.CngAlgorithm]::ECDsaP256) { throw 'KeyPath is not an ECDSA P-256 private key.' }
    $signer = [Security.Cryptography.ECDsaCng]::new($key)
    try {
        $signer.HashAlgorithm = [Security.Cryptography.CngAlgorithm]::Sha256
        $signature = $signer.SignData($manifestBytes)
    } finally { $signer.Dispose() }
} finally { $key.Dispose() }
if ($signature.Length -ne 64) { throw "Unexpected signature length $($signature.Length)." }
if (-not (Test-Signature $manifestBytes $signature $publicBlob)) {
    throw 'The signature does not verify with the public key (wrong private key?). Nothing was written.'
}

# ---- write, then verify what is on disk
$signatureText = [Convert]::ToBase64String($signature) + "`n"
try {
    [IO.File]::WriteAllBytes($manifestPath, $manifestBytes)
    [IO.File]::WriteAllText($signaturePath, $signatureText, [Text.UTF8Encoding]::new($false))
    $written = [IO.File]::ReadAllBytes($manifestPath)
    $writtenSignature = [Convert]::FromBase64String(([IO.File]::ReadAllText($signaturePath)).Trim())
    if (-not (Test-Signature $written $writtenSignature $publicBlob)) { throw 'The written manifest does not verify.' }
} catch {
    Remove-Item -LiteralPath $manifestPath, $signaturePath -Force -ErrorAction SilentlyContinue
    throw
}
$fingerprint = ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($publicBlob))).Replace('-', '').ToLowerInvariant()
[pscustomobject]@{
    manifest = $manifestPath; signature = $signaturePath; version = $Version; channel = $Channel
    setup_sha256 = $setup.sha256; portable_sha256 = $portable.sha256; public_key_sha256 = $fingerprint; verified = $true
    next = 'Attach overglaze-update.json and overglaze-update.json.sig to the GitHub release with the Setup and the ZIP.'
}
