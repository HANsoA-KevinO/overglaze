# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
[CmdletBinding()]
param([string]$BuildDirectory='',
      # The program root: <root>\data receives the run directory and the user's own
      # model is read in place from <root>\app\models\nvngx_dlssnr.dll, the same
      # layout rule the host applies to a fixture. Default: %OVERGLAZE_PROGRAM_ROOT%,
      # else the checkout this script sits in (<root>\source\powershell).
      [string]$ProgramRoot='',
      # Optional pins, recorded either way: refuse to run unless nvidia-smi reports
      # exactly this GPU name and/or driver version.
      [string]$ExpectedGpu='',
      [ValidatePattern('^(\d{3}\.\d{2})?$')][string]$ExpectedDriverVersion='',
      [switch]$ComputeOnly,
      [switch]$SuperResolution,
      # RE Engine guides (RE9): two-plane R32G8X24 depth, RG16F motion, cross-thread constants.
      [switch]$DepthStencil,
      # Binding preservation around the insertion (RE9 / 007 keep recording on their pre-Evaluate bindings).
      [switch]$BindingPreservation,
      # An ExecuteIndirect with a signature of unknown layout before the Evaluate (late attach, RE9). Implies binding preservation.
      [switch]$UnknownIndirect,
      # Depth and motion tagged eOnlyValidNow, overwritten after the tag, a Present before the Evaluate (2077 with DLSS-G on).
      [switch]$OnlyValidNow,
      [switch]$Retain)
# The NR controller host, executed. A synthetic game in an isolated run directory:
# the shipping controller dxgi.dll proxy, the controller bridge, and a harness
# that supplies only the PUBLIC Streamline surface. No game is launched, no game
# directory is read or written, nothing is published.
#
# It deliberately does NOT synthesise an installation, an adapter package or an
# sl.interposer.dll: the host's fixture path runs with no installation at all and
# re-checks this executable's name, directory and hash itself.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if($DepthStencil -and $SuperResolution){throw '-DepthStencil is an RR variant; do not combine with -SuperResolution'}
if($OnlyValidNow -and $DepthStencil){throw '-OnlyValidNow copies single-plane guides; do not combine with -DepthStencil'}
# No machine path: the program root is a parameter (see above), its data root is
# <root>\data, and the model is read in place from <root>\app\models.
if(-not $ProgramRoot){$ProgramRoot=if($env:OVERGLAZE_PROGRAM_ROOT){$env:OVERGLAZE_PROGRAM_ROOT}else{Split-Path (Split-Path $PSScriptRoot -Parent) -Parent}}
$labTree=[IO.Path]::GetFullPath($ProgramRoot)
$labRoot=Join-Path $labTree 'data'
if(-not (Test-Path -LiteralPath $labRoot)){New-Item -ItemType Directory -Path $labRoot | Out-Null}
if(-not $BuildDirectory){$BuildDirectory=Join-Path $labRoot '_build_controller'}
$labBuild=[IO.Path]::GetFullPath($BuildDirectory)
if(@(Get-Process 007FirstLight,AlanWake2,CONTROLResonant,Cyberpunk2077,DOOMTheDarkAges,FlightSimulator2024,HaloCampaignEvolved,Hellblade2-WinGDK-Shipping,LEGOBatmanLotDK-Win64-Shipping,OnimushaWotS,PRAGMATA,Resonance,SandFall-WinGDK-Shipping,TheGreatCircle,re9 -ErrorAction SilentlyContinue).Count){throw 'Exit the game normally; no concurrent GPU test'}
if(@(Get-Process lab_controller_harness,lab_standalone_harness -ErrorAction SilentlyContinue).Count){throw 'An earlier fixture is still running; inspect it first'}
if([IO.DriveInfo]::new([IO.Path]::GetPathRoot($labRoot)).AvailableFreeSpace -lt 30GB){throw '30 GiB reserve required'}
$labSmi=Join-Path $env:SystemRoot 'System32\nvidia-smi.exe'
if(-not (Test-Path -LiteralPath $labSmi)){throw 'An NVIDIA GPU with its driver is required (nvidia-smi.exe not found)'}
$labGpu=& $labSmi --query-gpu=name,driver_version --format=csv,noheader
if($LASTEXITCODE -ne 0 -or @($labGpu).Count -ne 1){throw 'Exactly one NVIDIA GPU is required for this fixture'}
$labGpuName,$labDriver=($labGpu.Trim() -split ',\s*',2)
if($ExpectedGpu -and $labGpuName -ne $ExpectedGpu){throw "GPU is '$labGpuName', expected '$ExpectedGpu'"}
if($ExpectedDriverVersion -and $labDriver -ne $ExpectedDriverVersion){throw "Driver is $labDriver, expected $ExpectedDriverVersion"}
$labSample=Join-Path $labTree 'app\models\nvngx_dlssnr.dll'
if(-not (Test-Path -LiteralPath $labSample)){throw "NR model missing: $labSample (the user supplies nvngx_dlssnr.dll; this project does not include it)"}
# The reviewed model versions, read from the same table the bridge and the
# manager use (core/include/lab_model_versions.hpp); the bridge re-checks.
$labVersions=Join-Path (Split-Path $PSScriptRoot -Parent) 'native\core\include\lab_model_versions.hpp'
$labKnown=@([regex]::Matches([IO.File]::ReadAllText($labVersions),'(?<![0-9a-f])[0-9a-f]{64}(?![0-9a-f])') | ForEach-Object Value)
if(-not $labKnown.Count){throw "No reviewed model version found in $labVersions"}
if((Get-FileHash -LiteralPath $labSample).Hash.ToLowerInvariant() -notin $labKnown){throw 'NR model is not a reviewed version'}
# Laid out exactly as an installed game is under the root strategy: the thin
# proxy beside the executable, and everything else in the payload subdirectory.
# The fixture therefore exercises the real bring-up order -- game loads the
# proxy, proxy loads the host at the first factory -- rather than a shape that
# only exists in tests. The model is read in place from app\models.
$labFiles=@{'dxgi.dll'=(Join-Path $labBuild 'controller\dxgi.dll');
    'overglaze\overglaze_controller.dll'=(Join-Path $labBuild 'controller\overglaze_controller.dll');
    'overglaze\overglaze_nvngx.dll'=(Join-Path $labBuild 'controller\overglaze_nvngx.dll');
    'lab_controller_harness.exe'=(Join-Path $labBuild 'lab_controller_harness.exe')}
foreach($labFile in $labFiles.Values){if(-not [IO.File]::Exists($labFile)){throw "Missing $labFile"}}
# Serialise with the other GPU work in this workspace.
$labMutex=[Threading.Mutex]::new($false,'Global\Overglaze-ctest')
if(-not $labMutex.WaitOne([TimeSpan]::FromMinutes(20))){throw 'Another lab GPU run holds the workspace mutex'}
try{
# Snapshot what already exists, so the stray-file check flags only what this run made.
$labBefore=@{settings=@(Get-ChildItem -Path (Join-Path $labRoot 'settings') -ErrorAction SilentlyContinue | ForEach-Object Name)
    daily=@(Get-ChildItem -Path $labRoot -Filter 'daily-*' -Directory -ErrorAction SilentlyContinue | ForEach-Object Name)}
$labRun=Join-Path $labRoot ('controller-host-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $labRun | Out-Null
$labCopies=@()
foreach($labName in $labFiles.Keys){$labTarget=Join-Path $labRun $labName
    $labParent=Split-Path -Parent $labTarget
    if(-not (Test-Path -LiteralPath $labParent)){New-Item -ItemType Directory -Path $labParent | Out-Null}
    [IO.File]::Copy($labFiles[$labName],$labTarget,$false)
    $labHash=(Get-FileHash -LiteralPath $labFiles[$labName]).Hash.ToLowerInvariant()
    if((Get-FileHash -LiteralPath $labTarget).Hash.ToLowerInvariant() -ne $labHash){throw 'Copy verification failed'}
    $labCopies+=@{path=$labTarget;source=$labFiles[$labName];sha256=$labHash;bytes=[IO.FileInfo]::new($labTarget).Length}}
$labFixtureHash=(Get-FileHash -LiteralPath (Join-Path $labRun 'lab_controller_harness.exe')).Hash.ToLowerInvariant()
$labEnv=@{OVERGLAZE_CONTROLLER_FIXTURE='1';OVERGLAZE_CONTROLLER_FIXTURE_SHA256=$labFixtureHash;
    # The bridge client's own fixture gate reads the standalone variable name.
    OVERGLAZE_STANDALONE_FIXTURE_SHA256=$labFixtureHash;
    OVERGLAZE_OFFLINE_CONFIRMED='1';OVERGLAZE_NO_ANTICHEAT_CONFIRMED='1';OVERGLAZE_ENABLE_CONTROLLED_NR='1';
    OVERGLAZE_LIVE_BRIDGE_SHA256=(Get-FileHash -LiteralPath (Join-Path $labRun 'overglaze\overglaze_nvngx.dll')).Hash.ToLowerInvariant();
    OVERGLAZE_LIVE_DATA_PATH=$labRun;OVERGLAZE_FIXTURE_BINDING_PRESERVATION=$(if($BindingPreservation -or $UnknownIndirect){'1'}else{'0'});OVERGLAZE_FIXTURE_UNKNOWN_INDIRECT=$(if($UnknownIndirect){'1'}else{'0'})}
$labReceipt=[ordered]@{purpose='functional-verification'
    question='Does the thin root proxy load the controller host from the payload subdirectory at the game''s first factory, and does that host admit a synthetic RR call on PUBLIC evidence only, load the CONTROLLER bridge variant and execute real NR, with no research observer, capture or probe in the binary?'
    acceptance='Host starts and publishes controller-d3d12; fixture attaches with no private callbacks; admission_mode public-evaluate-evidence-only; bridge_variant controller; NR ON changes the texture; the capability ladder reaches L3 with can_capture false; OFF leaves it byte-identical; no capture/probe/post/observer status is published; orderly stop. With -ComputeOnly it also runs NR without writing the game colour back and proves the texture stays byte-identical while evaluates keep rising.'
    origin='synthetic';gpu=$labGpu;expected_gpu=$ExpectedGpu;expected_driver=$ExpectedDriverVersion;game_started=$false;game_directory_touched=$false
    raw_texture_files=0;retained_raw_files=0;maximum_seconds=90;files=$labCopies;pid=0
    variant=$(if($OnlyValidNow){'only-valid-now+'}else{''})+$(if($UnknownIndirect){'unknown-indirect+'}else{''})+$(if($BindingPreservation -or $UnknownIndirect){'binding-preservation+'}else{''})+$(if($DepthStencil){$(if($ComputeOnly){'compute-only+depth-stencil'}else{'depth-stencil'})}elseif($ComputeOnly -and $SuperResolution){'compute-only+sr'}elseif($ComputeOnly){'compute-only'}elseif($SuperResolution){'sr'}else{'default'})
    host_sha256=(Get-FileHash -LiteralPath (Join-Path $labRun 'overglaze\overglaze_controller.dll')).Hash.ToLowerInvariant()
    bridge_sha256=$labEnv.OVERGLAZE_LIVE_BRIDGE_SHA256
    harness_sha256=$labFixtureHash}
function Save-Receipt{[IO.File]::WriteAllText((Join-Path $labRun 'launch.json'),($labReceipt|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))}
Save-Receipt
$labPrevious=@{}
try{foreach($labKey in $labEnv.Keys){$labPrevious[$labKey]=[Environment]::GetEnvironmentVariable($labKey,'Process');[Environment]::SetEnvironmentVariable($labKey,$labEnv[$labKey],'Process')}
    # Redirect through cmd.exe: PowerShell's own -RedirectStandardOutput yields a
    # Process whose ExitCode reads as $null under restricted hosts.
    $labExe=Join-Path $labRun 'lab_controller_harness.exe'
    $labReport=Join-Path $labRun 'controller-result.json'
    $labArgs='"'+$(if($ComputeOnly){' --compute-only'}else{''})+$(if($SuperResolution){' --sr'}else{''})+$(if($DepthStencil){' --depth-stencil'}else{''})+$(if($UnknownIndirect){' --unknown-indirect'}else{''})+$(if($OnlyValidNow){' --only-valid-now'}else{''})
    $labCommand='/d /c ""'+$labExe+'" "'+$labReport+$labArgs+' > "'+(Join-Path $labRun 'stdout.txt')+'" 2> "'+(Join-Path $labRun 'stderr.txt')+'""'
    $labProcess=Start-Process -FilePath 'cmd.exe' -ArgumentList $labCommand -WorkingDirectory $labRun -WindowStyle Hidden -PassThru
    $labReceipt.pid=$labProcess.Id;Save-Receipt
}finally{foreach($labKey in $labPrevious.Keys){[Environment]::SetEnvironmentVariable($labKey,$labPrevious[$labKey],'Process')}}
if(-not $labProcess.WaitForExit(90000)){throw "Fixture exceeded its time bound; inspect exact PID $($labProcess.Id), do not launch another instance"}
$labProcess.Refresh();$labReceipt.exit_code=$labProcess.ExitCode;Save-Receipt
if($null -eq $labProcess.ExitCode){throw "Fixture exit code unreadable; inspect $labRun before rerun"}
Get-Content -LiteralPath (Join-Path $labRun 'stdout.txt')
if($labProcess.ExitCode -ne 0){Get-Content -LiteralPath (Join-Path $labRun 'stderr.txt');throw "Failed controller fixture retained at $labRun"}
if(-not (Test-Path -LiteralPath $labReport)){throw "Controller fixture wrote no result; retained at $labRun"}
$labResult=Get-Content -LiteralPath $labReport -Raw -Encoding UTF8 | ConvertFrom-Json
if(-not $labResult.passed){throw "Controller fixture reported failure: $($labResult.error); retained at $labRun"}
if($labResult.admission.mode -ne 'public-evaluate-evidence-only'){throw "Controller admitted on unexpected evidence: $($labResult.admission.mode)"}
if($labResult.bridge.variant -ne 'controller'){throw "Controller host loaded the wrong bridge variant: $($labResult.bridge.variant)"}
if(-not $labResult.research_surface_absent){throw 'Controller host published a research status surface'}
if($labResult.nr_on.evaluates -lt 1){throw 'No NR evaluation was recorded'}
# The fixture must not have taken any installation path: no new daily run
# directory, no startup error record, and no new panel preferences. Compare
# against the snapshot taken before launch; the user's own game settings pre-exist.
$labAfter=@{settings=@(Get-ChildItem -Path (Join-Path $labRoot 'settings') -ErrorAction SilentlyContinue | ForEach-Object Name)
    daily=@(Get-ChildItem -Path $labRoot -Filter 'daily-*' -Directory -ErrorAction SilentlyContinue | ForEach-Object Name)}
foreach($labKind in @('settings','daily')){
    $labNew=@($labAfter[$labKind] | Where-Object {$labBefore[$labKind] -notcontains $_})
    if($labNew.Count){throw "Fixture created $labKind entries outside its run directory: $($labNew -join ', ')"}}
[pscustomobject]@{Run=$labRun;Passed=$true;Checks=$labResult.checks;NR_Evaluates=$labResult.nr_on.evaluates;
    AdmissionMode=$labResult.admission.mode;BridgeVariant=$labResult.bridge.variant}
if(-not $Retain){
    # Keep launch.json, controller-result.json, stdout.txt and stderr.txt: a
    # passing run is the evidence. Only the copied binaries go.
    foreach($labName in $labFiles.Keys){Remove-Item -LiteralPath (Join-Path $labRun $labName) -Force}
}
}finally{$labMutex.ReleaseMutex();$labMutex.Dispose()}
