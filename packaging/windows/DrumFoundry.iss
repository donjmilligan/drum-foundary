; Inno Setup script for the Windows installer: installers\Windows\DrumFoundry-<version>-Windows-Setup.exe.
;
; Usage (from the repo root, after a Release build):
;   ISCC.exe /DAppVersion=0.1.1 /DBuildDir=<absolute path to build\DrumFoundry_artefacts\Release> packaging\windows\DrumFoundry.iss
;
; Both defines are optional; the defaults below suit a local build in <repo>\build.
; Needs Inno Setup 6.3 or newer (the x64compatible architecture identifier).
;
; Installs:
;   VST3        -> {commoncf64}\VST3\Drum Foundry.vst3  (C:\Program Files\Common Files\VST3)
;   Standalone  -> {app}\Drum Foundry.exe              (C:\Program Files\Drum Foundry)
;   Licences    -> {app}\Licenses                      (always installed)
; With only the VST3 selected, {app} still holds the licences and the uninstaller.

#if Ver < EncodeVer(6, 3, 0)
  #error Inno Setup 6.3 or newer is required to compile this script
#endif

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

; SourcePath is the folder holding this script, so the defaults work from any
; working directory.
#define RepoDir AddBackslash(SourcePath) + "..\..\"

#ifndef BuildDir
  #define BuildDir RepoDir + "build\DrumFoundry_artefacts\Release"
#endif
#define Artefacts AddBackslash(BuildDir)

#define AppName "Drum Foundry"
#define AppExe "Drum Foundry.exe"
#define AppURL "https://github.com/donjmilligan/drum-foundary"

[Setup]
; Never change AppId: Windows uses it to find the installed copy for upgrades
; and uninstall. The doubled brace is Inno's escape for a literal "{".
AppId={{8AE2D06D-BED6-43A3-9E3F-8299060CA1A4}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Theres Tears Records
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases
AppCopyright=Copyright (c) 2026 Don Milligan
; Accepts up to four numbers (missing ones become 0), so the version must stay
; purely numeric, as CMake's project VERSION is.
VersionInfoVersion={#AppVersion}

; The binaries are x64 only. x64compatible also admits Arm64 Windows 11, which
; runs them under emulation.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; The VST3 goes to the machine-wide plug-in folder, which needs admin rights.
PrivilegesRequired=admin

DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
UninstallDisplayName={#AppName}
; JUCE embeds the app icon in the exe. With the standalone left out this file
; is missing and Windows picks a generic icon.
UninstallDisplayIcon={app}\{#AppExe}

LicenseFile={#RepoDir}LICENSE
; NOTICE carries the copyright, the AGPL section 7 terms and the third-party list.
InfoBeforeFile={#RepoDir}NOTICE

WizardStyle=modern
Compression=lzma2
SolidCompression=yes
OutputDir={#RepoDir}installers\Windows
OutputBaseFilename=DrumFoundry-{#AppVersion}-Windows-Setup

; A DAW holding the plug-in open blocks the update. .vst3 is added because the
; default filter only checks .exe, .dll and .chm files.
CloseApplications=yes
CloseApplicationsFilter=*.exe,*.dll,*.chm,*.vst3

; Code signing is off: it needs a code-signing certificate. To sign Setup and
; the uninstaller, define a sign tool on the ISCC command line, for example
;   ISCC.exe "/Ssigntool=signtool.exe sign /a /fd sha256 /tr http://timestamp.digicert.com /td sha256 $f" ...
; and uncomment the line below. Sign the exe and the .vst3 binary in the build
; step, before packaging.
;SignTool=signtool

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Full installation"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plug-in"; Types: full
Name: "standalone"; Description: "Standalone app"; Types: full

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Components: standalone; Flags: unchecked

[Files]
; The VST3 is a folder bundle; copy all of it.
Source: "{#Artefacts}VST3\Drum Foundry.vst3\*"; DestDir: "{commoncf64}\VST3\Drum Foundry.vst3"; Excludes: "*.pdb,*.ilk,*.exp,*.lib"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Artefacts}Standalone\{#AppExe}"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion

; .txt names so they open on double-click.
Source: "{#RepoDir}LICENSE"; DestDir: "{app}\Licenses"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#RepoDir}NOTICE"; DestDir: "{app}\Licenses"; DestName: "NOTICE.txt"; Flags: ignoreversion
Source: "{#RepoDir}libs\CloudSeedCore\LICENSE"; DestDir: "{app}\Licenses"; DestName: "CloudSeedCore-LICENSE.txt"; Flags: ignoreversion
Source: "{#RepoDir}assets\fonts\Inter-OFL.txt"; DestDir: "{app}\Licenses"; Flags: ignoreversion
Source: "{#RepoDir}assets\fonts\Archivo-OFL.txt"; DestDir: "{app}\Licenses"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; Components: standalone
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Components: standalone; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Components: standalone; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Installed files go automatically; this also takes anything left in the bundle,
; which belongs to this product alone.
Type: filesandordirs; Name: "{commoncf64}\VST3\Drum Foundry.vst3"; Components: vst3
