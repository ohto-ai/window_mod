; Inno Setup script for Window Modifier
; Build: iscc installer\window_mod.iss

#define MyAppName "Window Modifier"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "ohto-ai"
#define MyAppURL "https://github.com/ohto-ai/window_mod"
#define MyAppExeName "window_mod.exe"

[Setup]
AppId={{A3F7B2D1-5C4E-4A9F-B3D2-8E1C6F0A7B4D}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
; Only allow install on x64-compatible systems (x64 and ARM64 Windows 11)
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
PrivilegesRequiredOverridesAllowed=dialog
OutputDir=Output
OutputBaseFilename=WindowModifierInstaller
SetupIconFile=..\src\window_mod.ico
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\dist\*.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\*.dll"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
