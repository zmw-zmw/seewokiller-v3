; SeewoKiller v3 - Inno Setup Installer Script
; Compatible with Inno Setup 5 and 6
; Usage: Open in Inno Setup IDE and click Build
; All paths are relative to this file, so the script works from any location.

#define MyAppName "SeewoKiller"
#define MyAppVersion "3.0.0"
#define MyAppPublisher "SeewoKiller"
#define MyAppExeName "SeewoKiller.exe"

[Setup]
AppId={{B7E3D9A1-4F2C-4A8E-9D1B-3E5F7A2C8B64}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={userpf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=installer
OutputBaseFilename=SeewoKiller-v3-Setup
SetupIconFile=res\app.ico
LicenseFile=LICENSE.rtf
Compression=lzma2/ultra64
SolidCompression=yes
PrivilegesRequired=lowest
UninstallDisplayName={#MyAppName} {#MyAppVersion}
DisableProgramGroupPage=yes

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; Main executable
Source: "build\Release\SeewoKiller.exe"; DestDir: "{app}"; Flags: ignoreversion

; Runtime DLLs
Source: "build\Release\ssh.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\WebView2Loader.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\Microsoft.Web.WebView2.Core.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\libcrypto-3-x64.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\libssl-3-x64.dll"; DestDir: "{app}"; Flags: ignoreversion

; VC++ runtime DLLs (for systems without vc_redist installed)

; Resources (vnc.html + noVNC + icon)
Source: "build\Release\res\vnc.html"; DestDir: "{app}\res"; Flags: ignoreversion
Source: "res\app.ico"; DestDir: "{app}\res"; DestName: "app.ico"; Flags: ignoreversion
Source: "build\Release\res\novnc\*"; DestDir: "{app}\res\novnc"; Flags: ignoreversion recursesubdirs createallsubdirs

; Plugins (.pfl - one-click command fill buttons)
Source: "plugins\*.pfl"; DestDir: "{app}\plugins"; Flags: ignoreversion

; License file
Source: "LICENSE.rtf"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\res\app.ico"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\res\app.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\res\webview_data"
