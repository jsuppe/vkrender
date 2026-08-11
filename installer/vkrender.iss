; M25: vkrender presentation appliance installer (Inno Setup 6).
;
; Installs the renderer, every receiver (Miracast / WebRTC / AirPlay), the
; virtual camera (DirectShow + optional Media Foundation source, both
; registered here), the producer client DLL and shaders, then writes
; vkrender.ini from the wizard answers.
;
; Build:  iscc installer\vkrender.iss     (after a Release build of the repo)
; Output: installer\Output\vkrender-setup-<version>.exe

#define MyAppName "vkrender"
#define MyAppVersion "0.28.0"
#define MyAppPublisher "vkrender"
#define BuildDir "..\build\Release"
#define AirplayDir "..\..\vkrender-airplay\build\Release"

[Setup]
AppId={{7E4B1C92-3F5A-4E8D-9A21-6C0B7D2E1F44}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\vkrender
DefaultGroupName=vkrender
OutputDir=Output
OutputBaseFilename=vkrender-setup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; M28: install WITHOUT elevation, and ask for it only when a selected feature
; genuinely needs it.
;
; Inno bakes PrivilegesRequired into the setup EXE's manifest, so `admin` makes
; Windows show UAC before a single line of our code runs - unconditionally,
; whatever the user picked. `lowest` means no prompt at launch; {autopf} then
; resolves to %LOCALAPPDATA%\Programs instead of Program Files, which needs no
; rights at all. The two privileged steps (registering the camera DLLs, and the
; optional Wireless Display feature) are done afterwards by ONE elevated child
; process, and only if those features were selected. See RunPrivilegedSetup.
;
; /ALLUSERS still forces the classic machine-wide install for anyone who wants
; it (that path does prompt, by definition).
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
UninstallDisplayName={#MyAppName} presentation appliance

[Types]
Name: "full"; Description: "Everything (receivers + virtual camera)"
Name: "receivers"; Description: "Receivers only (no virtual camera)"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "core"; Description: "Renderer + appliance launcher"; Types: full receivers custom; Flags: fixed
Name: "webrtc"; Description: "WebRTC / WHIP receiver (Flutter sender, browsers, OBS)"; Types: full receivers custom
; Miracast RECEIVING needs a Wi-Fi driver that provides the sink role - many
; adapters (incl. some current Wi-Fi 6E parts) support projecting only. Check
; Settings > System > Projecting to this PC before enabling this. Off by
; default in the standard install types so nobody gets a receiver that cannot
; work; still selectable under "Custom". See docs/MIRACAST.md.
Name: "miracast"; Description: "Miracast receiver (needs a Wi-Fi adapter that can RECEIVE Miracast - see Settings > Projecting to this PC)"; Types: custom
Name: "airplay"; Description: "AirPlay receiver (Apple senders, GPLv3)"; Types: full receivers custom; Check: AirplayAvailable
; M27: RTSP is the odd one out - it PULLS from cameras/encoders rather than
; waiting to be discovered, so it does nothing until URLs are configured in
; vkrender.ini (or POSTed to its control API). Installed with the receivers,
; started only when rtsp=1.
Name: "rtsp"; Description: "RTSP receiver (pull from IP cameras / encoders)"; Types: full receivers custom
; The two camera components are the ONLY things here that need administrator,
; and for different reasons:
;   dshow - DllRegisterServer writes COM keys under HKCR (HKLM-backed);
;           BUILTIN\Users has ReadKey only.
;   mf    - same, PLUS the DLL is loaded by the Camera Frame Server running as
;           LocalService in session 0, which cannot read %LOCALAPPDATA%. So the
;           elevated step also copies it to {commonappdata} (machine-readable)
;           and registers it from there.
; Every receiver, including Miracast, installs and runs with no elevation.
Name: "vcam"; Description: "Virtual camera (scene as a webcam)"; Types: full custom
Name: "vcam\dshow"; Description: "DirectShow camera - Chrome, OBS, Zoom, classic Teams  [requires administrator]"; Types: full custom
Name: "vcam\mf"; Description: "Media Foundation camera - also Windows Camera, new Teams  [requires administrator]"; Types: full custom

[Files]
; --- core ---
Source: "{#BuildDir}\vkrenderer.exe"; DestDir: "{app}"; Components: core; Flags: ignoreversion
Source: "{#BuildDir}\vkr_appliance.exe"; DestDir: "{app}"; Components: core; Flags: ignoreversion
Source: "{#BuildDir}\shaders\*"; DestDir: "{app}\shaders"; Components: core; Flags: ignoreversion recursesubdirs
; --- receivers ---
Source: "{#BuildDir}\vkr_webrtc.exe"; DestDir: "{app}"; Components: webrtc; Flags: ignoreversion
Source: "{#BuildDir}\vkr_miracast.exe"; DestDir: "{app}"; Components: miracast; Flags: ignoreversion
Source: "{#AirplayDir}\vkr_airplay.exe"; DestDir: "{app}"; Components: airplay; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#BuildDir}\vkr_rtsp.exe"; DestDir: "{app}"; Components: rtsp; Flags: ignoreversion
; --- virtual camera ---
; framelink.dll is NOT optional: both vkrenderer.exe and framecam.exe link
; it, so it ships with the base install whatever components are chosen. It
; lives in a sibling `framelink` checkout now, but our CMake co-locates it
; into BuildDir so packaging stays a single directory.
Source: "{#BuildDir}\framelink.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\framecam.exe"; DestDir: "{app}"; Components: vcam; Flags: ignoreversion
; No `regserver` here: that flag makes Inno register in-process, which forces
; the WHOLE installer to be elevated. RunPrivilegedSetup does it afterwards in
; a single elevated child, only when these components are selected.
Source: "{#BuildDir}\softcam.dll"; DestDir: "{app}"; Components: vcam\dshow; Flags: ignoreversion
Source: "{#BuildDir}\vkrender_vcam_source.dll"; DestDir: "{app}"; Components: vcam\mf; Flags: ignoreversion
; --- docs ---
Source: "..\README.md"; DestDir: "{app}"; DestName: "README.md"; Flags: ignoreversion

[Icons]
Name: "{group}\Start vkrender appliance"; Filename: "{app}\vkr_appliance.exe"; WorkingDir: "{app}"
Name: "{group}\Edit settings (vkrender.ini)"; Filename: "notepad.exe"; Parameters: """{app}\vkrender.ini"""
Name: "{group}\Uninstall vkrender"; Filename: "{uninstallexe}"
Name: "{autodesktop}\vkrender appliance"; Filename: "{app}\vkr_appliance.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts"
; Miracast receiving builds on Windows' "Wireless display" optional feature
; (it supplies the sink infrastructure). Offered - never forced: it is an OS
; feature that downloads from Microsoft, may be blocked on managed machines,
; and also enables "Projecting to this PC".
Name: "wirelessdisplay"; Description: "Install Windows' ""Wireless display"" feature (needed to receive Miracast; downloads from Microsoft)"; GroupDescription: "Miracast prerequisite"; Components: miracast; Check: WirelessDisplayMissing

[Run]
Filename: "{app}\vkr_appliance.exe"; Description: "Start the vkrender appliance now"; Flags: postinstall nowait skipifsilent

; NOTE: no [UninstallRun] for the camera cleanup. The uninstaller is no longer
; elevated either, so unregistering the machine-wide camera is done from
; CurUninstallStepChanged via the same elevate-once helper - and only when
; something was actually registered. See docs/VIRTUAL_CAMERA.md.

[Code]
var
  ConfigPage: TInputOptionWizardPage;
  PinPage: TInputQueryWizardPage;
  ElevatePage: TInputOptionWizardPage; // consent, shown only when needed
  AdminNoteLabel: TNewStaticText;      // live hint on the components page
  PrivilegedOK: Boolean;               // did the elevated step succeed?
  PrivilegedRan: Boolean;

// Line break helper: a source line may not START with '#' (the Inno
// preprocessor claims those), which makes inline #13#10 awkward.
function NL(): String;
begin
  Result := Chr(13) + Chr(10);
end;

// '1'/'0' for vkrender.ini (Pascal Script needs helpers declared first).
function BoolToStr2(B: Boolean): String;
begin
  if B then Result := '1' else Result := '0';
end;

// The AirPlay receiver ships from the separate GPLv3 repo; its [Files] entry
// is skipifsourcedoesntexist, so offering the component is always safe - it
// simply installs nothing when that build isn't present.
function AirplayAvailable(): Boolean;
begin
  Result := True;
end;

// GStreamer runtime is required by the WebRTC and AirPlay receivers.
function GStreamerInstalled(): Boolean;
begin
  Result := DirExists('C:\gstreamer\1.0\msvc_x86_64\bin')
         or DirExists('C:\Program Files\gstreamer\1.0\msvc_x86_64\bin');
end;

// Is the "Wireless display" FOD absent? (Any query failure -> treat as
// missing, so we offer the option rather than silently skipping it.)
function WirelessDisplayMissing(): Boolean;
var
  Code: Integer;
begin
  Result := True;
  if Exec('powershell.exe',
          '-NoProfile -ExecutionPolicy Bypass -Command "if ((Get-WindowsCapability -Online ' +
          '-Name App.WirelessDisplay.Connect*).State -eq ''Installed'') { exit 0 } else { exit 1 }"',
          '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Result := (Code <> 0);
end;

// ---- which selections need administrator -----------------------------------
// Kept in one place so the components hint, the consent page and the elevated
// step can never disagree about what is privileged.

function NeedsDshowReg(): Boolean;
begin
  Result := WizardIsComponentSelected('vcam\dshow');
end;

function NeedsMfReg(): Boolean;
begin
  Result := WizardIsComponentSelected('vcam\mf');
end;

function NeedsWirelessDisplay(): Boolean;
begin
  Result := WizardIsTaskSelected('wirelessdisplay');
end;

function NeedsElevation(): Boolean;
begin
  Result := NeedsDshowReg() or NeedsMfReg() or NeedsWirelessDisplay();
end;

// Human-readable list of exactly what will run elevated, for the consent page
// and for the "you declined" message. No vague "some features need admin".
function PrivilegedWorkSummary(): String;
begin
  Result := '';
  if NeedsDshowReg() then
    Result := Result + '  - Register the DirectShow camera (softcam.dll) for all users' + NL();
  if NeedsMfReg() then
    Result := Result + '  - Copy the Media Foundation camera to ' +
              ExpandConstant('{commonappdata}') + '\vkrender and register it' + NL() +
              '    (the Camera Frame Server runs as LocalService and cannot read' + NL() +
              '     a per-user folder, so this one cannot stay under your profile)' + NL();
  if NeedsWirelessDisplay() then
    Result := Result + '  - Install Windows'' "Wireless display" feature (downloads from Microsoft)' + NL();
end;

// Live hint under the components list.
procedure UpdateAdminNote();
begin
  if AdminNoteLabel = nil then Exit;
  if NeedsElevation() then
  begin
    AdminNoteLabel.Caption :=
      'Your selection includes features that need administrator. Windows will ask' + NL() +
      'for permission once, after the files are copied - everything else installs' + NL() +
      'either way.';
    AdminNoteLabel.Font.Color := clHighlight;
  end
  else
    AdminNoteLabel.Caption :=
      'Nothing selected needs administrator - this installs entirely under your' + NL() +
      'user account, with no Windows permission prompt.';
end;

procedure ComponentsChanged(Sender: TObject);
begin
  UpdateAdminNote();
end;

// Do every privileged step in ONE elevated child, so the user sees at most a
// single UAC prompt for the whole install. The work is written to a .ps1 first
// - composing it as an inline -Command string is a quoting minefield, and a
// file is also something the user can inspect or re-run by hand later.
//
// Returns False if elevation was declined or the script reported failure; the
// caller degrades gracefully rather than failing the install.
function RunPrivilegedSetup(): Boolean;
var
  Script, Marker, Shared, Lines: String;
  Code: Integer;
begin
  Result := True;
  if not NeedsElevation() then Exit;

  Script := ExpandConstant('{app}\vkr_privileged_setup.ps1');
  Marker := ExpandConstant('{app}\vkr_privileged_setup.done');
  Shared := ExpandConstant('{commonappdata}\vkrender');
  DeleteFile(Marker);

  Lines := '$ErrorActionPreference = ''Stop''' + NL() +
           '$ok = $true' + NL();

  if NeedsDshowReg() then
    Lines := Lines +
      'try { regsvr32.exe /s "' + ExpandConstant('{app}\softcam.dll') + '" }' + NL() +
      'catch { $ok = $false }' + NL();

  if NeedsMfReg() then
    Lines := Lines +
      // The Frame Server (LocalService, session 0) cannot read %LOCALAPPDATA%,
      // so this DLL has to live somewhere machine-readable.
      'try {' + NL() +
      '  New-Item -ItemType Directory -Force -Path "' + Shared + '" | Out-Null' + NL() +
      '  Copy-Item -Force "' + ExpandConstant('{app}\vkrender_vcam_source.dll') + '" "' +
        Shared + '\vkrender_vcam_source.dll"' + NL() +
      '  regsvr32.exe /s "' + Shared + '\vkrender_vcam_source.dll"' + NL() +
      '} catch { $ok = $false }' + NL();

  if NeedsWirelessDisplay() then
    Lines := Lines +
      // Non-fatal on its own: offline / WSUS / managed machines fail here and
      // that must not cost the user the camera registrations above.
      'try { Add-WindowsCapability -Online -Name App.WirelessDisplay.Connect~~~~0.0.1.0 | Out-Null }' + NL() +
      'catch { }' + NL();

  Lines := Lines +
    'if ($ok) { Set-Content -Path "' + Marker + '" -Value ''ok'' -Encoding ascii }' + NL();

  if not SaveStringToFile(Script, Lines, False) then
  begin
    Result := False;
    Exit;
  end;

  WizardForm.StatusLabel.Caption := 'Waiting for administrator approval...';
  // 'runas' is what raises UAC from this unelevated process.
  if not ShellExec('runas', 'powershell.exe',
                   '-NoProfile -ExecutionPolicy Bypass -File "' + Script + '"',
                   '', SW_HIDE, ewWaitUntilTerminated, Code) then
  begin
    Result := False;   // declined, or the shell refused to launch it
    Exit;
  end;
  Result := FileExists(Marker);
end;

procedure InitializeWizard();
begin
  ConfigPage := CreateInputOptionPage(wpSelectTasks,
    'Appliance settings', 'How should the presentation appliance behave?',
    'These are written to vkrender.ini and can be changed later.',
    False, False);
  ConfigPage.Add('Require a PIN before a sender may present');
  ConfigPage.Add('Use a fixed PIN instead of a new code per connection');
  ConfigPage.Values[0] := True;
  ConfigPage.Values[1] := False;

  PinPage := CreateInputQueryPage(ConfigPage.ID,
    'Fixed PIN', 'Choose the PIN senders must enter',
    'Digits only. Leave blank to generate a new code for each connection.');
  PinPage.Add('PIN:', False);
  PinPage.Values[0] := '';

  // Consent page - only reached when the selection actually needs admin.
  ElevatePage := CreateInputOptionPage(wpReady,
    'Administrator permission',
    'Some of the features you selected need administrator rights',
    'Everything else has already been prepared under your user account and will' + NL() +
    'install regardless. Only the steps below need elevation:' + NL(),
    False, False);
  ElevatePage.Add('I understand - show the Windows permission prompt');
  ElevatePage.Values[0] := True;

  // Live admin hint under the components list.
  AdminNoteLabel := TNewStaticText.Create(WizardForm);
  AdminNoteLabel.Parent := WizardForm.SelectComponentsPage;
  AdminNoteLabel.Left := WizardForm.ComponentsList.Left;
  AdminNoteLabel.Width := WizardForm.ComponentsList.Width;
  AdminNoteLabel.Top := WizardForm.ComponentsList.Top +
                        WizardForm.ComponentsList.Height + ScaleY(4);
  AdminNoteLabel.AutoSize := False;
  AdminNoteLabel.Height := ScaleY(40);
  WizardForm.ComponentsList.Height := WizardForm.ComponentsList.Height - ScaleY(44);
  AdminNoteLabel.Top := WizardForm.ComponentsList.Top +
                        WizardForm.ComponentsList.Height + ScaleY(4);
  WizardForm.ComponentsList.OnClickCheck := @ComponentsChanged;
  UpdateAdminNote();
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpSelectComponents then UpdateAdminNote();
  // Fill the consent page with the exact commands that will run elevated.
  if (ElevatePage <> nil) and (CurPageID = ElevatePage.ID) then
    ElevatePage.SubCaptionLabel.Caption :=
      'Everything else has already been prepared under your user account and will' + NL() +
      'install regardless. Only these steps need elevation:' + NL() + NL() +
      PrivilegedWorkSummary();
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if PageID = PinPage.ID then
    Result := (not ConfigPage.Values[0]) or (not ConfigPage.Values[1]);
  if (ElevatePage <> nil) and (PageID = ElevatePage.ID) then
    Result := not NeedsElevation();
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectComponents then
  begin
    if (WizardIsComponentSelected('webrtc') or WizardIsComponentSelected('airplay'))
       and (not GStreamerInstalled()) then
    begin
      if MsgBox('The GStreamer MSVC x86_64 runtime was not found.' + NL + NL +
                'The WebRTC and AirPlay receivers need it. Install it from' + NL +
                'https://gstreamer.freedesktop.org/download/ (runtime MSI),' + NL +
                'then start the appliance.' + NL + NL +
                'Continue installing anyway?', mbConfirmation, MB_YESNO) = IDNO then
        Result := False;
    end;
  end;
end;

// Write vkrender.ini from the wizard answers.
procedure CurStepChanged(CurStep: TSetupStep);
var
  Ini, Pin, VcamMode: String;
  VcamActive: Boolean;
begin
  if CurStep <> ssPostInstall then Exit;

  // Everything privileged happens here, in one elevated child, and only if the
  // selection needs it. Declining costs exactly those features and nothing
  // else - the receivers and renderer are already installed and working.
  PrivilegedRan := NeedsElevation();
  if PrivilegedRan then
  begin
    PrivilegedOK := RunPrivilegedSetup();
    if not PrivilegedOK then
      MsgBox('Administrator permission was declined or the privileged step failed.' + NL + NL +
             'Everything that does not need it is installed and working:' + NL +
             'the renderer, the appliance launcher, and every receiver you selected' + NL +
             '(WebRTC, AirPlay, Miracast, RTSP).' + NL + NL +
             'Not active:' + NL + PrivilegedWorkSummary() + NL +
             'To enable them later, right-click this file and Run as administrator:' + NL +
             '  ' + ExpandConstant('{app}\vkr_privileged_setup.ps1'),
             mbInformation, MB_OK);
  end;

  Ini := ExpandConstant('{app}\vkrender.ini');

  if not ConfigPage.Values[0] then
    Pin := 'off'
  else if ConfigPage.Values[1] and (Trim(PinPage.Values[0]) <> '') then
    Pin := Trim(PinPage.Values[0])
  else
    Pin := 'auto';

  if WizardIsComponentSelected('vcam\mf') then VcamMode := 'mf' else VcamMode := 'dshow';

  // An unregistered camera is a camera that does not work, so do not leave the
  // launcher starting framecam into a guaranteed failure. Re-running
  // vkr_privileged_setup.ps1 elevated and setting virtual_camera=1 turns it on.
  VcamActive := WizardIsComponentSelected('vcam') and
                ((not PrivilegedRan) or PrivilegedOK);

  SetIniString('appliance', 'name', 'vkrender', Ini);
  SetIniString('appliance', 'pin', Pin, Ini);
  SetIniString('appliance', 'layout', 'presentation', Ini);
  SetIniString('appliance', 'window', '', Ini);
  SetIniString('appliance', 'webrtc_port', '8889', Ini);
  SetIniString('appliance', 'virtual_camera', BoolToStr2(VcamActive), Ini);
  SetIniString('appliance', 'vcam_mode', VcamMode, Ini);
  SetIniString('appliance', 'airplay', BoolToStr2(WizardIsComponentSelected('airplay')), Ini);
  SetIniString('appliance', 'miracast', BoolToStr2(WizardIsComponentSelected('miracast')), Ini);
  // RTSP ships enabled-but-idle: installing the component is not the same as
  // wanting it running, because it only does anything once URLs exist. Set
  // rtsp=1 and list rtsp_urls (or POST to the control API) to start pulling.
  SetIniString('appliance', 'rtsp', '0', Ini);
  SetIniString('appliance', 'rtsp_port', '8890', Ini);
  SetIniString('appliance', 'rtsp_urls', '', Ini);
end;

// Uninstall is unelevated too, so the machine-wide bits need the same
// elevate-once treatment - and only if they were ever registered. A
// system-lifetime MF camera outlives uninstall otherwise, leaving a ghost
// camera in every app's device list (see docs/VIRTUAL_CAMERA.md).
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Script, Shared, Lines: String;
  Code: Integer;
  NeedDshow, NeedMf: Boolean;
begin
  if CurUninstallStep <> usUninstall then Exit;

  Shared := ExpandConstant('{commonappdata}\vkrender');
  NeedMf := FileExists(Shared + '\vkrender_vcam_source.dll');
  NeedDshow := FileExists(ExpandConstant('{app}\softcam.dll'));
  if not (NeedMf or NeedDshow) then Exit;

  Script := ExpandConstant('{app}\vkr_privileged_uninstall.ps1');
  Lines := '$ErrorActionPreference = ''SilentlyContinue''' + NL();
  if NeedMf then
    Lines := Lines +
      '& "' + ExpandConstant('{app}\framecam.exe') + '" --unregister' + NL() +
      'regsvr32.exe /s /u "' + Shared + '\vkrender_vcam_source.dll"' + NL() +
      'Remove-Item -Recurse -Force "' + Shared + '"' + NL();
  if NeedDshow then
    Lines := Lines +
      'regsvr32.exe /s /u "' + ExpandConstant('{app}\softcam.dll') + '"' + NL();

  if not SaveStringToFile(Script, Lines, False) then Exit;
  if not ShellExec('runas', 'powershell.exe',
                   '-NoProfile -ExecutionPolicy Bypass -File "' + Script + '"',
                   '', SW_HIDE, ewWaitUntilTerminated, Code) then
    MsgBox('The virtual camera could not be unregistered (administrator permission' + NL +
           'was declined), so it may still appear in camera lists.' + NL + NL +
           'To remove it, right-click this file and Run as administrator:' + NL +
           '  ' + Script, mbInformation, MB_OK);
end;
