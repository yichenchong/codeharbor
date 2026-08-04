; CodeHarbor Windows installer, built with Inno Setup 6 (ISCC).
;
; WHY INNO SETUP AND NOT WIX/MSI
; Both are present on GitHub's windows-latest image (InnoSetup 6.7.1, WiX
; Toolset 3.14). This application is a single unmanaged executable plus a tree
; of Qt/WebEngine/libssh DLLs and resource directories that `windeployqt`
; produces, with no services, no COM registration and no shared components. For
; that shape Inno Setup is one declarative script with a recursive [Files] entry,
; whereas WiX needs a generated component/GUID per payload file (heat harvesting)
; that has to be regenerated whenever the Qt deploy output changes, plus an
; upgrade table to get the same in-place upgrade this script gets from AppId
; alone. Inno Setup also does a genuinely non-elevated per-user install out of
; the box; a per-user MSI is possible but is the awkward path.
;
; WHAT PICKING INNO SETUP GIVES UP: no .msi, so no Group Policy / Intune / SCCM
; software deployment, no msiexec administrative install or transforms, and no
; MSI self-repair. Managed corporate environments that mandate MSI are therefore
; not served by this artifact; if that becomes a requirement, a WiX MSI should be
; added ALONGSIDE this installer, not instead of it, because the per-user
; unelevated path is the one that matters for unsigned artifacts.
;
; NO ADMINISTRATOR RIGHTS BY DEFAULT: the release artifacts are unsigned, and an
; unsigned installer that raises a UAC prompt is a far worse first impression
; than one that never asks. PrivilegesRequired=lowest keeps the whole install
; inside the user's profile. PrivilegesRequiredOverridesAllowed=commandline lets
; someone deploying for every user on a machine pass /ALLUSERS explicitly,
; without adding a prompt to the normal path.
;
; VERSION: this script deliberately contains NO version literal. The single
; source of truth is the project(... VERSION x.y.z ...) line in the top-level
; CMakeLists.txt (mirrored into the npm manifests by
; .omp/skills/bump-version/bump.sh); the release workflow parses it and passes it
; in as /DAppVersion. If that define is missing the compile fails below rather
; than silently shipping a wrong or stale version.
;
; This file is checked out with LF line endings (see `* text=auto eol=lf` in
; .gitattributes). That is fine: ISCC's script reader breaks a line on either
; #10 or #13 and skips a #10 that follows a #13
; (issrc Projects/Src/Shared.FileClass.pas, TTextFileReader.DoReadLine), so no
; CRLF conversion step is needed.

#ifndef AppVersion
  #error AppVersion is not defined. Compile with /DAppVersion=X.Y.Z, taken from the project() VERSION in the top-level CMakeLists.txt.
#endif

#ifndef SourceDir
  #error SourceDir is not defined. Compile with /DSourceDir=<directory holding codeharbor.exe and the windeployqt output>.
#endif

; Where the compiled installer lands. Defaults to the script's own directory so
; a hand-run on a developer's Windows box works without extra defines.
#ifndef OutputDir
  #define OutputDir SourcePath
#endif

#define AppName "CodeHarbor"
#define AppPublisher "CodeHarbor contributors"
#define AppUrl "https://github.com/yichenchong/codeharbor"
#define AppExeName "codeharbor.exe"
; SourcePath (this script's directory) carries NO trailing backslash, which is
; why every path built from it goes through AddBackslash.
#define ScriptDir AddBackslash(SourcePath)
#define IconFile ScriptDir + "..\codeharbor.ico"
#define LicenseFilePath ScriptDir + "..\..\LICENSE"

[Setup]
; AppId is the identity Windows and Inno Setup use to recognise an existing
; installation. It MUST stay byte-for-byte stable across releases forever: this
; single value is what makes an upgrade replace the previous version in place
; (same install directory, same Start-menu entry, one row in "Installed apps")
; instead of installing a second copy beside it. Never regenerate it.
AppId={{6B1C2A74-3E4D-4F58-9A0B-7C5D8E2F1A93}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
; Metadata stamped into the setup executable's own version resource, so the file
; properties of the download and the "Installed apps" row agree.
VersionInfoVersion={#AppVersion}
VersionInfoProductVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} {#AppVersion} installer
VersionInfoCopyright=Copyright (c) 2026 CodeHarbor contributors

; Per-user install, no elevation. {autopf} resolves to
; %LOCALAPPDATA%\Programs in this mode and to Program Files under /ALLUSERS.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
; UsePreviousAppDir (on by default, stated for the record) is what sends an
; upgrade to wherever the previous version was installed; DisableDirPage=auto
; then skips asking, so an upgrade is Next-Next with no decisions.
UsePreviousAppDir=yes
UsePreviousTasks=yes
DisableDirPage=auto
DisableProgramGroupPage=yes
AllowNoIcons=yes
LicenseFile={#LicenseFilePath}

; Qt 6.9 and WebEngine require Windows 10 or newer, 64-bit.
MinVersion=10.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; A running instance holds its DLLs open, which would otherwise turn an upgrade
; into a "files in use, reboot required" dead end. Ask (via the Restart Manager)
; to close it, and do not silently relaunch it afterwards.
CloseApplications=yes
RestartApplications=no

Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupIconFile={#IconFile}
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\codeharbor.ico
OutputDir={#OutputDir}
OutputBaseFilename=CodeHarbor-{#AppVersion}-windows-x64-setup

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
; Shown by CurUninstallStepChanged below. Settings are kept unless the user
; actively opts out, so an uninstall never silently discards server profiles.
RemoveSettingsPrompt=Also delete your CodeHarbor settings (saved server profiles, window state) and cached data?%n%nChoose No to keep them for a future reinstall.

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; The whole deployed tree: codeharbor.exe, the Qt DLLs, the QML modules, the
; Qt WebEngine runtime and locales, and the libssh DLL. Recursive by design -
; the exact file list is whatever windeployqt produced and must not be enumerated
; here, or a Qt upgrade would silently drop files from the installer.
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; Shipped so the shortcuts and the "Installed apps" row have a real icon: the
; executable itself carries no icon resource.
Source: "{#IconFile}"; DestDir: "{app}"; DestName: "codeharbor.ico"; Flags: ignoreversion

[InstallDelete]
; Upgrade hygiene. [Files] overwrites, but it never removes a file that a NEWER
; version no longer ships - and the Qt deploy output changes shape between Qt
; releases, so an upgraded install would accumulate orphaned DLLs and, worse,
; could load a stale Qt plugin left behind from the previous version.
;
; Clearing {app} is only safe if {app} is the previous install directory. The
; Check below requires both the executable and the stable AppId uninstall record
; to point at this exact path. On a fresh install, or if the user points the
; installer at some directory of their own, nothing is deleted.
Type: filesandordirs; Name: "{app}\*"; Check: PreviousInstallPresent

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"; IconFilename: "{app}\codeharbor.ico"; Comment: "Manage persistent remote development workspaces"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; IconFilename: "{app}\codeharbor.ico"; Comment: "Manage persistent remote development workspaces"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Everything installed is removed by the uninstaller already; this only drops the
; install directory itself once it is empty (Qt writes nothing there at runtime -
; its caches live under %LOCALAPPDATA% - so it should be).
Type: dirifempty; Name: "{app}"

[Code]
// True when the target directory is the existing CodeHarbor install directory.
// The executable check alone was unsafe: a user could select a directory that
// happened to contain an unrelated codeharbor.exe and the [InstallDelete] sweep
// would erase every file in it. The AppId-specific registry path is written by
// Inno Setup for every install and is stable across upgrades.
function PreviousInstallPresent: Boolean;
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6B1C2A74-3E4D-4F58-9A0B-7C5D8E2F1A93}_is1';
var
  PreviousPath: String;
  AppPath: String;
begin
  Result := False;
  AppPath := AddBackslash(ExpandConstant('{app}'));
  if not FileExists(AppPath + '{#AppExeName}') then
    Exit;

  if RegQueryStringValue(HKEY_CURRENT_USER, UninstallKey,
                         'Inno Setup: App Path', PreviousPath) and
     (CompareText(AddBackslash(PreviousPath), AppPath) = 0) then
    begin
      Result := True;
      Exit;
    end;
  if RegQueryStringValue(HKEY_LOCAL_MACHINE, UninstallKey,
                         'Inno Setup: App Path', PreviousPath) and
     (CompareText(AddBackslash(PreviousPath), AppPath) = 0) then
    Result := True;
end;

// Per-user settings live in the registry, NOT in the install directory: the
// client stores them through Qt's QSettings with organisation "CodeHarbor" and
// application "CodeHarbor" (see src/app/ServerProfiles.h), which on Windows is
// HKEY_CURRENT_USER\Software\CodeHarbor\CodeHarbor. Nothing in this script
// touches that key during an install, so an upgrade preserves saved server
// profiles and window state automatically.
//
// On uninstall we ASK, defaulting to No, rather than deleting silently: losing
// your server profiles because you reinstalled is not an acceptable surprise. A
// silent uninstall (/SILENT, /VERYSILENT) always keeps them - there is nobody to
// answer the question.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  SettingsKey: String;
  CacheDir: String;
begin
  if CurUninstallStep <> usPostUninstall then
    Exit;
  if UninstallSilent then
    Exit;

  SettingsKey := 'Software\CodeHarbor\CodeHarbor';
  CacheDir := ExpandConstant('{localappdata}\CodeHarbor');
  if not RegKeyExists(HKEY_CURRENT_USER, SettingsKey) and not DirExists(CacheDir) then
    Exit;

  if MsgBox(CustomMessage('RemoveSettingsPrompt'), mbConfirmation,
            MB_YESNO or MB_DEFBUTTON2) = IDYES then
  begin
    RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, SettingsKey);
    // Qt WebEngine profile storage and the QML disk cache, under
    // %LOCALAPPDATA%\CodeHarbor\CodeHarbor. Disposable, and only removed on the
    // same explicit opt-in as the settings.
    DelTree(CacheDir, True, True, True);
  end;
end;
