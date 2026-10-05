# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [switch]$SkipTests,
    [switch]$SkipPython,
    # Only the CPU / WARP tests (ctest -LE gpu): no hardware GPU needed.
    [switch]$NoGpuTests,
    # Default: <checkout>\data\_build_controller (the out-of-game programs find
    # their program root from a <root>\data\_build* directory).
    [string]$BuildDirectory = '',
    # Default: %OVERGLAZE_TEST_LOG_DIR% when set, else <build>\logs.
    [string]$LogDirectory = ''
)
# NR Controller track: LAB_TRACK=controller configures core/runtime/provider/viewer/controller
# only (native/research is never added). Tests: ctest -L controller plus
# source\tests\controller. GPU/window tests are serialised across build
# directories through the named mutex Global\Overglaze-ctest.
$ErrorActionPreference='Stop'
$source=Split-Path $PSScriptRoot -Parent
$root=Split-Path $source -Parent
$build=if($BuildDirectory){[IO.Path]::GetFullPath($BuildDirectory)}else{Join-Path $root 'data\_build_controller'}
. (Join-Path $PSScriptRoot 'Enter-Environment.ps1') | Out-Null
$report=if($LogDirectory){$LogDirectory}elseif($env:OVERGLAZE_TEST_LOG_DIR){$env:OVERGLAZE_TEST_LOG_DIR}else{Join-Path $build 'logs'}
$env:PYTHONDONTWRITEBYTECODE='1'
$env:VSLANG='1033'
& cmake -S $source -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLAB_TRACK=controller
if($LASTEXITCODE -ne 0){throw 'Configure failed'}
& cmake --build $build --parallel 4
if($LASTEXITCODE -ne 0){throw 'Build failed'}
if(-not $SkipTests){
    New-Item -ItemType Directory -Force -Path $report | Out-Null
    if(-not $SkipPython){
        $log=Join-Path $report 'controller-python-tests.txt'
        $env:OVERGLAZE_CONTROLLER_BUILD=$build
        & cmd.exe /d /c "python -B -m unittest discover -s `"$source\tests\controller`" -q > `"$log`" 2>&1"
        $pythonExit=$LASTEXITCODE
        Get-Content -LiteralPath $log -Tail 4
        if($pythonExit -ne 0){throw 'Python regression (controller) failed'}
    }
    $mutex=[System.Threading.Mutex]::new($false,'Global\Overglaze-ctest')
    if(-not $mutex.WaitOne([TimeSpan]::FromMinutes(20))){throw 'Another Overglaze ctest run holds Global\Overglaze-ctest'}
    try{
        $log=Join-Path $report 'controller-native-tests.txt'
        $exclude=if($NoGpuTests){'-LE gpu'}else{''}
        & cmd.exe /d /c "ctest --test-dir `"$build`" -L controller $exclude --output-on-failure > `"$log`" 2>&1"
        $ctestExit=$LASTEXITCODE
        Get-Content -LiteralPath $log | Select-String -Pattern 'tests passed|Total Test time|Failed|Not Run' | ForEach-Object { $_.Line }
        if($ctestExit -ne 0){throw 'Native regression (controller) failed'}
    }finally{$mutex.ReleaseMutex();$mutex.Dispose()}
}
Write-Output "Controller track built in $build. No game started, no NR evaluation or installation performed."
