# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
Exercise the actual installer in a disposable local folder, without games or GPU.
.DESCRIPTION
Refuses any existing registration for this AppId. The only uninstall it invokes
belongs to the fresh test directory. Persistent content is synthetic; no model
or game is copied. The test leaves its small receipt and logs for review.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SetupExecutable,
    [Parameter(Mandatory=$true)][ValidatePattern('^\{[A-Fa-f0-9-]{36}\}$')][string]$AppId,
    [string]$RepositoryRoot=''
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(-not $RepositoryRoot){$RepositoryRoot=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent}
$RepositoryRoot=[IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\')
$SetupExecutable=[IO.Path]::GetFullPath($SetupExecutable)
if(-not (Test-Path -LiteralPath $SetupExecutable -PathType Leaf)){throw 'Setup executable missing'}
$testParent=Join-Path $RepositoryRoot 'data\_installer-tests'
function Check-OrdinaryPath([string]$Path){
    $cursor=[IO.Path]::GetFullPath($Path)
    while($cursor){
        $item=Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
        if($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw "Reparse point refused: $cursor"}
        $next=[IO.Path]::GetDirectoryName($cursor);if($next -eq $cursor){break};$cursor=$next
    }
}
Check-OrdinaryPath $testParent;Check-OrdinaryPath $SetupExecutable
$registrationPath='Software\Microsoft\Windows\CurrentVersion\Uninstall\'+$AppId+'_is1'
foreach($view in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)){
    $hive=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,$view)
    try{$key=$hive.OpenSubKey($registrationPath);if($key){$key.Dispose();throw 'An existing installation owns this AppId; test refused'}}finally{$hive.Dispose()}
}
$testDirectory=Join-Path $testParent ('cycle-'+[guid]::NewGuid().ToString('N'))
$installRoot=Join-Path $testDirectory 'installed app 中文'
$checks=[Collections.Generic.List[string]]::new()
New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
function Save-Text([string]$Path,[string]$Value){
    New-Item -ItemType Directory -Path (Split-Path $Path -Parent) -Force | Out-Null
    [IO.File]::WriteAllText($Path,$Value,[Text.UTF8Encoding]::new($false))
}
Save-Text (Join-Path $testDirectory 'PURPOSE.json') '{"purpose":"installer-cycle-functional-test","synthetic":true,"game_started":false,"nr_executed":false}'
function Require([bool]$Condition,[string]$Name){if(-not $Condition){throw "Installer cycle failed: $Name"};$checks.Add($Name)}
function Run-Setup([string]$Log){
    $process=Start-Process -FilePath $SetupExecutable -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/NOLAUNCH','/NOICONS','/TASKS=""',('/DIR="'+$installRoot+'"'),('/LOG="'+$Log+'"')) -WindowStyle Hidden -PassThru -Wait
    return $process.ExitCode
}
function Check-App([string]$Operation,[int]$Expected){
    $text=& (Join-Path $installRoot 'app\overglaze_games.exe') app-check $Operation --root $installRoot
    $code=$LASTEXITCODE
    Require ($code -eq $Expected) ("app-check "+$Operation+" exit "+$Expected)
    return ($text -join [Environment]::NewLine | ConvertFrom-Json)
}
function Own-Uninstall(){
    $hive=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
    try{
        $key=$hive.OpenSubKey($registrationPath)
        if(-not $key){throw 'Test registration missing; no uninstaller launched'}
        try{
            $location=[string]$key.GetValue('InstallLocation')
            if($location.TrimEnd('\') -ne $installRoot.TrimEnd('\')){throw 'Uninstall registration points outside the fixture'}
        }finally{$key.Dispose()}
    }finally{$hive.Dispose()}
    Check-OrdinaryPath $installRoot
    $uninstaller=Join-Path $installRoot 'app\unins000.exe'
    if(-not (Test-Path -LiteralPath $uninstaller)){$uninstaller=Join-Path $installRoot 'unins000.exe'}
    Require (Test-Path -LiteralPath $uninstaller -PathType Leaf) 'own uninstaller exists'
    $process=Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/LOG="'+(Join-Path $testDirectory 'uninstall.log')+'"')) -WindowStyle Hidden -PassThru -Wait
    return $process.ExitCode
}
$passed=$false;$failure='';$uninstalled=$false
try{
    Require ((Run-Setup (Join-Path $testDirectory 'install.log')) -eq 0) 'fresh installation'
    Require (Test-Path -LiteralPath (Join-Path $installRoot 'app\overglaze_viewer.exe')) 'viewer installed'
    $null=Check-App 'uninstall' 0
    $sentinels=@('data/settings/test-retained.json','data/captures/test-retained.txt','app/models/test-retained.bin','app/adapters/test-retained/test.txt')
    foreach($relative in $sentinels){Save-Text (Join-Path $installRoot $relative) ('Synthetic user content: '+$relative)}
    $sentinelHashes=@{};foreach($relative in $sentinels){$sentinelHashes[$relative]=(Get-FileHash -LiteralPath (Join-Path $installRoot $relative)).Hash}
    Require ((Run-Setup (Join-Path $testDirectory 'upgrade.log')) -eq 0) 'same-directory update'
    foreach($relative in $sentinels){Require ((Get-FileHash -LiteralPath (Join-Path $installRoot $relative)).Hash -eq $sentinelHashes[$relative]) ('update retains '+$relative)}
    # A process genuinely mapped from this test app folder. It expires itself.
    $holder=Join-Path $installRoot 'app\test-process.exe'
    Copy-Item -LiteralPath (Join-Path $env:WINDIR 'System32\PING.EXE') -Destination $holder
    $holdingProcess=Start-Process -FilePath $holder -ArgumentList @('127.0.0.1','-n','8','-w','500') -WindowStyle Hidden -PassThru
    try{$null=Check-App 'update' 2}finally{if(-not $holdingProcess.WaitForExit(12000)){throw 'Synthetic holder did not exit; not force-closing it'}}
    Remove-Item -LiteralPath $holder
    # A malformed pending record must block the actual uninstaller as well.
    $blockingFile=Join-Path $installRoot 'data\settings\plugin-manager\fixture-blocked\transaction.json'
    Save-Text $blockingFile '{"state":"installed","fixture":true}'
    $null=Check-App 'uninstall' 2
    Require ((Own-Uninstall) -ne 0) 'uninstaller refuses unresolved game record'
    Require (Test-Path -LiteralPath (Join-Path $installRoot 'app\overglaze_viewer.exe')) 'blocked uninstall keeps program'
    Remove-Item -LiteralPath $blockingFile
    $null=Check-App 'uninstall' 0
    Require ((Own-Uninstall) -eq 0) 'uninstall completes'
    $uninstalled=$true
    Require (-not (Test-Path -LiteralPath (Join-Path $installRoot 'app\overglaze_viewer.exe'))) 'packaged viewer removed'
    foreach($relative in $sentinels){Require ((Get-FileHash -LiteralPath (Join-Path $installRoot $relative)).Hash -eq $sentinelHashes[$relative]) ('uninstall retains '+$relative)}
    $passed=$true
}catch{$failure=$_.Exception.Message}
finally{
    $receipt=[ordered]@{schema='overglaze-installer-cycle-v1';passed=$passed;error=$failure;checks=@($checks);setup_sha256=(Get-FileHash -LiteralPath $SetupExecutable -Algorithm SHA256).Hash.ToLowerInvariant();fixture=$installRoot;uninstalled=$uninstalled;game_started=$false;nr_executed=$false;gpu_used=$false}
    Save-Text (Join-Path $testDirectory 'result.json') (($receipt|ConvertTo-Json -Depth 6)+[Environment]::NewLine)
}
[pscustomobject]@{Passed=$passed;Checks=$checks.Count;Receipt=Join-Path $testDirectory 'result.json';Fixture=$installRoot}
if(-not $passed){throw $failure}
