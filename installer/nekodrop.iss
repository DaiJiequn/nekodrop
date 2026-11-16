; nekodrop setup (Inno Setup 6). Built by scripts\build-installer.ps1 from build\stage.
#define AppVersion "0.1.0"

[Setup]
AppId={{6C1F2E7A-4B8E-4E57-9C61-5E2B1D0A9F31}
AppName=nekodrop
AppVersion={#AppVersion}
AppPublisher=nekodrop project
DefaultDirName={autopf}\nekodrop
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=nekodrop-{#AppVersion}-setup
OutputDir=..\build
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\nekodrop.exe

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "..\build\stage\*"; DestDir: "{tmp}\nekodrop-stage"; Flags: recursesubdirs ignoreversion deleteafterinstall

[Icons]
; The AppUserModelID lets Windows show nekodrop's notifications under its own name.
Name: "{autoprograms}\nekodrop"; Filename: "{app}\nekodrop.exe"; AppUserModelID: "nekodrop.app"

[Run]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{tmp}\nekodrop-stage\install.ps1"" -Target ""{app}"""; \
  StatusMsg: "Installing the driver, service and firewall rules..."; Flags: runhidden waituntilterminated
Filename: "{app}\nekodrop.exe"; Description: "Start nekodrop"; Flags: postinstall nowait skipifsilent runasoriginaluser

[UninstallRun]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\uninstall.ps1"" -Target ""{app}"""; \
  Flags: runhidden waituntilterminated; RunOnceId: "nekodrop-uninstall"
