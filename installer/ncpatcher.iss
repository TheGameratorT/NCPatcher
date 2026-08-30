; Inno Setup script for NCPatcher.
;
; Built from a staged portable install, so the installer ships exactly what
; `cmake --install` produces with NCP_PORTABLE_LAYOUT=ON: the binary and its
; data files in one directory. That is also what the release zip contains, so
; the two cannot drift apart.
;
; Build with:
;   cmake --install build --prefix stage
;   iscc /DStageDir=..\stage /DAppVersion=1.0.4 installer\ncpatcher.iss

#ifndef StageDir
  #define StageDir "..\stage"
#endif
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{6C0A4E63-3B4B-4E3F-96C4-1E1D2E9E0A11}
AppName=NCPatcher
AppVersion={#AppVersion}
AppPublisher=TheGameratorT
AppPublisherURL=https://github.com/TheGameratorT/NCPatcher
DefaultDirName={autopf}\NCPatcher
DefaultGroupName=NCPatcher
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\ncpatcher.exe
OutputDir=..\dist
OutputBaseFilename=ncpatcher-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile={#StageDir}\LICENSE.txt

[Tasks]
; The reason this installer exists. Adding the install directory to PATH by
; hand, and then rebooting for it to take, was the step the project templates
; had to spell out; a checkbox removes it.
Name: "addtopath"; Description: "Add NCPatcher to the system PATH"; GroupDescription: "Command line:"

[Files]
Source: "{#StageDir}\ncpatcher.exe"; DestDir: "{app}"; Flags: ignoreversion
; ncp.h and its companions sit beside the binary, which is where the portable
; layout looks for them.
Source: "{#StageDir}\ncp.h"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\ncp_ide.h"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\ncprt.c"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\ncpatcher.schema.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
; Appended to the machine PATH, and removed again on uninstall. Inno's own
; environment change is broadcast to the shell, so a newly opened terminal sees
; it without logging out, which is the other half of deleting the reboot step.
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; \
    ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; \
    Check: NeedsAddPath(ExpandConstant('{app}')); Tasks: addtopath

[Code]
function NeedsAddPath(Dir: string): Boolean;
var
  Path: string;
begin
  if not RegQueryStringValue(HKLM,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', 'Path', Path) then
  begin
    Result := True;
    exit;
  end;
  // Semicolons either side, so that a directory whose name merely ends with
  // this one does not count as already present.
  Result := Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Path) + ';') = 0;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Path: string;
  Entry: string;
  P: Integer;
begin
  if CurUninstallStep <> usPostUninstall then
    exit;
  if not RegQueryStringValue(HKLM,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', 'Path', Path) then
    exit;

  Entry := ';' + ExpandConstant('{app}');
  P := Pos(Uppercase(Entry), Uppercase(Path));
  if P > 0 then
  begin
    Delete(Path, P, Length(Entry));
    RegWriteExpandStringValue(HKLM,
      'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', 'Path', Path);
  end;
end;
