; NTSC Snow — OBS plugin installer (Inno Setup 6)
;
; Installs the plugin into an existing OBS Studio installation:
;   ntsc-snow.dll -> <OBS>\obs-plugins\64bit\
;   data\*        -> <OBS>\data\obs-plugins\ntsc-snow\
;
; Build with:  installer\build-installer.ps1   (passes /DAppVersion from buildspec.json)
; or manually: ISCC /DAppVersion=1.0.0 installer\ntsc-snow.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

#define AppName "NTSC Snow (OBS plugin)"
#define AppPublisher "abanum"
#define AppURL "https://github.com/abanum/ZAA"

; Install tree produced by: cmake --install build_x64 --prefix release\RelWithDebInfo
#define PayloadDir "..\release\RelWithDebInfo\ntsc-snow"

[Setup]
AppId={{7E1F2A54-9C0B-4E2E-9C3D-4E2B6C0A1D77}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
DefaultDirName={code:GetOBSDir}
DefaultGroupName=NTSC Snow
DisableProgramGroupPage=yes
DisableWelcomePage=no
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\bin\64bit\obs64.exe
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
CloseApplications=yes
CloseApplicationsFilter=obs64.exe
OutputDir={#SourcePath}\..\release
OutputBaseFilename=ntsc-snow-{#AppVersion}-windows-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
DirExistsWarning=no

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ja"; MessagesFile: "compiler:Languages\Japanese.isl"

[Files]
Source: "{#PayloadDir}\bin\64bit\ntsc-snow.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "{#PayloadDir}\data\*"; DestDir: "{app}\data\obs-plugins\ntsc-snow"; Flags: recursesubdirs createallsubdirs ignoreversion

[UninstallDelete]
Type: filesandordirs; Name: "{app}\data\obs-plugins\ntsc-snow"

[Code]
{ Detect the OBS Studio install directory from the registry, else Program Files. }
function GetOBSDir(Param: String): String;
var
  dir: String;
begin
  if RegQueryStringValue(HKLM, 'SOFTWARE\OBS Studio', '', dir) and (dir <> '') then
    Result := dir
  else if RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\OBS Studio', '', dir) and (dir <> '') then
    Result := dir
  else
    Result := ExpandConstant('{autopf}\obs-studio');
end;

{ Warn if the chosen folder does not look like an OBS install. }
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectDir then begin
    if not FileExists(ExpandConstant('{app}\bin\64bit\obs64.exe')) then begin
      if MsgBox('選択したフォルダに obs64.exe が見つかりません。OBS Studio のインストール先を' + #13#10 +
                '指定してください（例: C:\Program Files\obs-studio）。' + #13#10#13#10 +
                'このまま続けますか？ / obs64.exe was not found here. Continue anyway?',
                mbConfirmation, MB_YESNO) = IDNO then
        Result := False;
    end;
  end;
end;
