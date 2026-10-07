; SPDX-FileCopyrightText: 2026 HANsoA-KevinO
; SPDX-License-Identifier: MIT
; Compile through Build-Installer.ps1: it verifies the incoming release and
; writes an explicit, hashed [Files] include. No wildcard copies are allowed.
#if Ver < EncodeVer(6, 7, 0, 0) || Ver >= EncodeVer(7, 0, 0, 0)
  #error Inno Setup 6.7 or newer in the 6.x series is required
#endif
#ifndef PayloadDirectory
  #error Build-Installer.ps1 must supply PayloadDirectory
#endif
#ifndef PayloadFiles
  #error Build-Installer.ps1 must supply PayloadFiles
#endif
#ifndef PackageVersion
  #error Build-Installer.ps1 must supply PackageVersion
#endif
#ifndef IconPath
  #error Build-Installer.ps1 must supply IconPath
#endif

[Setup]
AppId={{66B920DD-3026-4D7D-AC73-64E349940B95}
AppName=Overglaze
AppVersion={#PackageVersion}
AppVerName=Overglaze {#PackageVersion}
AppPublisher=HANsoA-KevinO
AppPublisherURL=https://github.com/HANsoA-KevinO/overglaze
AppSupportURL=https://github.com/HANsoA-KevinO/overglaze/issues
AppUpdatesURL=https://github.com/HANsoA-KevinO/overglaze/releases
DefaultDirName={localappdata}\Programs\Overglaze
DefaultGroupName=Overglaze
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
UsePreviousAppDir=yes
UsePreviousGroup=yes
DisableDirPage=no
DisableProgramGroupPage=yes
DisableWelcomePage=no
CloseApplications=no
RestartApplications=no
RestartIfNeededByRun=no
AlwaysRestart=no
SetupLogging=yes
WizardStyle=modern dynamic windows11
WizardSizePercent=110
SetupIconFile={#IconPath}
UninstallDisplayIcon={app}\app\overglaze_viewer.exe
UninstallDisplayName=Overglaze
UninstallFilesDir={app}\app
OutputBaseFilename=Overglaze-Setup-{#PackageVersion}-win64
Compression=lzma2
SolidCompression=yes
LicenseFile={#PayloadDirectory}\LICENSE

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "{#AddBackslash(SourcePath)}ChineseSimplified.isl"

[CustomMessages]
english.DesktopShortcut=Create a desktop shortcut
english.LaunchProgram=Open Overglaze
english.WelcomeLabel=Install Overglaze, then add your games from the application. Your NVIDIA model is imported separately.
english.FixedRoot=Overglaze is already installed at %1. Update it at that location so existing game links keep working. To move it, first remove its game plugins and uninstall the application.
english.GuardFailed=Overglaze could not verify whether this operation is safe. No application files have been changed. Open Overglaze to resolve its game installations, then try again.
english.Blocked=Overglaze cannot continue yet:
english.NoHelper=Overglaze's installation checker is missing. Restore the application files before trying to uninstall.
english.Preserved=Your settings, captures, imported model and game recovery records are kept.
chinesesimplified.DesktopShortcut=创建桌面快捷方式
chinesesimplified.LaunchProgram=打开 Overglaze
chinesesimplified.WelcomeLabel=安装 Overglaze 后，在应用中添加游戏。NVIDIA 模型将在首次使用时单独导入。
chinesesimplified.FixedRoot=Overglaze 已安装在 %1。请在原位置更新，以保留现有游戏的关联。需要移动时，请先卸载游戏插件，再卸载应用。
chinesesimplified.GuardFailed=无法确认当前是否可以安全执行。应用文件尚未改动。请打开 Overglaze 处理游戏安装状态后重试。
chinesesimplified.Blocked=暂时无法继续：
chinesesimplified.NoHelper=应用安装检查程序缺失。请先恢复应用文件，再尝试卸载。
chinesesimplified.Preserved=设置、采集、导入的模型和游戏恢复记录将会保留。

[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopShortcut}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
#include PayloadFiles

[Icons]
Name: "{group}\Overglaze"; Filename: "{app}\app\overglaze_viewer.exe"; WorkingDir: "{app}"; AppUserModelID: "HANsoA-KevinO.Overglaze"
Name: "{autodesktop}\Overglaze"; Filename: "{app}\app\overglaze_viewer.exe"; WorkingDir: "{app}"; AppUserModelID: "HANsoA-KevinO.Overglaze"; Tasks: desktopicon

[Run]
Filename: "{app}\app\overglaze_viewer.exe"; Description: "{cm:LaunchProgram}"; Flags: nowait postinstall skipifsilent unchecked; Check: MayLaunch

[Code]
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{66B920DD-3026-4D7D-AC73-64E349940B95}_is1';
var
  PreviousRoot: String;
  HelperExtracted: Boolean;

function MayLaunch: Boolean;
var
  I: Integer;
begin
  Result := not WizardSilent;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), '/NOLAUNCH') = 0 then
      Result := False;
end;

function SameRoot(const Left, Right: String): Boolean;
begin
  Result := CompareText(RemoveBackslashUnlessRoot(ExpandFileName(Left)),
    RemoveBackslashUnlessRoot(ExpandFileName(Right))) = 0;
end;

function PreviousRootError: String;
begin
  Result := '';
  if (PreviousRoot <> '') and not SameRoot(WizardDirValue, PreviousRoot) then
    Result := FmtMessage(CustomMessage('FixedRoot'), [PreviousRoot]);
end;

procedure InitializeWizard;
begin
  PreviousRoot := '';
  RegQueryStringValue(HKCU, UninstallKey, 'InstallLocation', PreviousRoot);
  WizardForm.WelcomeLabel2.Caption := CustomMessage('WelcomeLabel');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Error: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then begin
    Error := PreviousRootError;
    if Error <> '' then begin
      SuppressibleMsgBox(Error, mbError, MB_OK, IDOK);
      Result := False;
    end;
  end;
end;

procedure ExtractChecker;
begin
  if HelperExtracted then exit;
  ExtractTemporaryFile('overglaze_games.exe');
  ExtractTemporaryFile('concrt140.dll');
  ExtractTemporaryFile('msvcp140.dll');
  ExtractTemporaryFile('msvcp140_1.dll');
  ExtractTemporaryFile('msvcp140_2.dll');
  ExtractTemporaryFile('msvcp140_atomic_wait.dll');
  ExtractTemporaryFile('msvcp140_codecvt_ids.dll');
  ExtractTemporaryFile('vccorlib140.dll');
  ExtractTemporaryFile('vcruntime140.dll');
  ExtractTemporaryFile('vcruntime140_1.dll');
  ExtractTemporaryFile('vcruntime140_threads.dll');
  HelperExtracted := True;
end;

function Guard(const Checker, Root, Operation: String): String;
var
  Code, TextCode, I: Integer;
  Output, TextOutput: TExecOutput;
  Started: Boolean;
  Detail: String;
begin
  Result := CustomMessage('GuardFailed');
  if not FileExists(Checker) then exit;
  try
    Started := ExecAndCaptureOutput(Checker,
      'app-check ' + Operation + ' --root "' + Root + '"',
      ExtractFileDir(Checker), SW_HIDE, ewWaitUntilTerminated, Code, Output);
    if not Started or Output.Error then exit;
    if Code = 0 then begin
      Result := '';
      exit;
    end;
    // Exit 2 is an expected block. Keep the decision from the JSON call, and
    // request its human-readable explanation without shell redirection.
    if Code <> 2 then exit;
    if ExecAndCaptureOutput(Checker,
      'app-check ' + Operation + ' --root "' + Root + '" --installer-text',
      ExtractFileDir(Checker), SW_HIDE, ewWaitUntilTerminated, TextCode, TextOutput)
      and not TextOutput.Error and (TextCode = 2) then begin
      Detail := '';
      for I := 0 to GetArrayLength(TextOutput.StdOut) - 1 do begin
        if Detail <> '' then Detail := Detail + #13#10;
        Detail := Detail + TextOutput.StdOut[I];
      end;
      if Trim(Detail) <> '' then
        Result := CustomMessage('Blocked') + #13#10#13#10 + Detail;
    end;
  except
    Log('Application preflight failed: ' + GetExceptionMessage);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  NeedsRestart := False;
  Result := PreviousRootError;
  if Result <> '' then exit;
  try
    ExtractChecker;
    Result := Guard(ExpandConstant('{tmp}\overglaze_games.exe'),
      RemoveBackslashUnlessRoot(WizardDirValue), 'update');
  except
    Result := CustomMessage('GuardFailed');
    Log(GetExceptionMessage);
  end;
end;

function JsonString(const Value: String): String;
var
  I, N: Integer;
  Hex: String;
begin
  Result := '"';
  Hex := '0123456789abcdef';
  for I := 1 to Length(Value) do begin
    N := Ord(Value[I]);
    if (N < 32) or (N > 126) then
      Result := Result + '\u' + Hex[(N div 4096) + 1] +
        Hex[((N div 256) mod 16) + 1] + Hex[((N div 16) mod 16) + 1] + Hex[(N mod 16) + 1]
    else if (Value[I] = '"') or (Value[I] = '\') then
      Result := Result + '\' + Value[I]
    else
      Result := Result + Value[I];
  end;
  Result := Result + '"';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  MarkerDirectory, Marker, Error: String;
begin
  if CurStep = ssInstall then begin
    // Recheck immediately before any writes, including silent installation.
    Error := Guard(ExpandConstant('{tmp}\overglaze_games.exe'),
      RemoveBackslashUnlessRoot(WizardDirValue), 'update');
    if Error <> '' then RaiseException(Error);
  end;
  if CurStep = ssPostInstall then begin
    MarkerDirectory := ExpandConstant('{app}\data\settings');
    if not ForceDirectories(MarkerDirectory) then
      RaiseException('Cannot create application settings directory.');
    Marker := '{"schema":"overglaze-application-install-v1","product":"overglaze",' +
      '"root":' + JsonString(RemoveBackslashUnlessRoot(ExpandConstant('{app}'))) +
      ',"version":"{#PackageVersion}"}' + #13#10;
    // JsonString emits ASCII escapes, so this is valid UTF-8 for every path.
    if not SaveStringToFile(MarkerDirectory + '\application-install.json', Marker, False) then
      RaiseException('Cannot save application installation identity.');
  end;
end;

function InitializeUninstall: Boolean;
var
  Error: String;
begin
  if not FileExists(ExpandConstant('{app}\app\overglaze_games.exe')) then
    Error := CustomMessage('NoHelper')
  else
    Error := Guard(ExpandConstant('{app}\app\overglaze_games.exe'),
      RemoveBackslashUnlessRoot(ExpandConstant('{app}')), 'uninstall');
  Result := Error = '';
  if not Result then
    SuppressibleMsgBox(Error, mbError, MB_OK, IDOK);
end;
