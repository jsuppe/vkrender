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
#define MyAppVersion "0.25.0"
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
; The virtual camera DLLs register machine-wide (HKCR), so we need admin.
PrivilegesRequired=admin
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
Name: "vcam"; Description: "Virtual camera (scene as a webcam)"; Types: full custom
Name: "vcam\dshow"; Description: "DirectShow camera (Chrome, OBS, Zoom, classic Teams)"; Types: full custom
Name: "vcam\mf"; Description: "Media Foundation camera (also Windows Camera, new Teams)"; Types: full custom

[Files]
; --- core ---
Source: "{#BuildDir}\vkrenderer.exe"; DestDir: "{app}"; Components: core; Flags: ignoreversion
Source: "{#BuildDir}\vkr_appliance.exe"; DestDir: "{app}"; Components: core; Flags: ignoreversion
Source: "{#BuildDir}\vkrender_client.dll"; DestDir: "{app}"; Components: core; Flags: ignoreversion
Source: "{#BuildDir}\shaders\*"; DestDir: "{app}\shaders"; Components: core; Flags: ignoreversion recursesubdirs
; --- receivers ---
Source: "{#BuildDir}\vkr_webrtc.exe"; DestDir: "{app}"; Components: webrtc; Flags: ignoreversion
Source: "{#BuildDir}\vkr_miracast.exe"; DestDir: "{app}"; Components: miracast; Flags: ignoreversion
Source: "{#AirplayDir}\vkr_airplay.exe"; DestDir: "{app}"; Components: airplay; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#BuildDir}\vkr_rtsp.exe"; DestDir: "{app}"; Components: rtsp; Flags: ignoreversion
; --- virtual camera ---
Source: "{#BuildDir}\vkr_vcam.exe"; DestDir: "{app}"; Components: vcam; Flags: ignoreversion
Source: "{#BuildDir}\softcam.dll"; DestDir: "{app}"; Components: vcam\dshow; Flags: ignoreversion regserver
Source: "{#BuildDir}\vkrender_vcam_source.dll"; DestDir: "{app}"; Components: vcam\mf; Flags: ignoreversion regserver
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

[UninstallRun]
; The MF camera registers system-wide (MFVirtualCameraLifetime_System) so Teams
; Rooms and friends can persist it as a content camera. That registration
; outlives uninstall unless it is removed - do it here, while still elevated.
; See docs/VIRTUAL_CAMERA.md.
Filename: "{app}\vkr_vcam.exe"; Parameters: "--unregister"; Flags: runhidden skipifdoesntexist; RunOnceId: "vcamunregister"

[Code]
var
  ConfigPage: TInputOptionWizardPage;
  PinPage: TInputQueryWizardPage;

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
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if PageID = PinPage.ID then
    Result := (not ConfigPage.Values[0]) or (not ConfigPage.Values[1]);
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
  Code: Integer;
begin
  if CurStep <> ssPostInstall then Exit;

  // Optional prerequisite, only when the user ticked it. Non-fatal: a
  // failure here (no internet, WSUS policy, managed device) must not fail
  // the install - the other two receivers do not need it.
  if WizardIsTaskSelected('wirelessdisplay') then
  begin
    WizardForm.StatusLabel.Caption := 'Installing the Windows "Wireless display" feature...';
    if not Exec('powershell.exe',
                '-NoProfile -ExecutionPolicy Bypass -Command "Add-WindowsCapability -Online ' +
                '-Name App.WirelessDisplay.Connect~~~~0.0.1.0"',
                '', SW_HIDE, ewWaitUntilTerminated, Code) or (Code <> 0) then
      MsgBox('The "Wireless display" feature could not be installed automatically' + NL +
             '(common on offline or company-managed machines).' + NL + NL +
             'Miracast receiving will not work until it is present. To install it' + NL +
             'later, run this in an elevated PowerShell:' + NL + NL +
             '  Add-WindowsCapability -Online -Name App.WirelessDisplay.Connect~~~~0.0.1.0'
             + NL + NL + 'AirPlay and WebRTC receiving are unaffected.',
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

  SetIniString('appliance', 'name', 'vkrender', Ini);
  SetIniString('appliance', 'pin', Pin, Ini);
  SetIniString('appliance', 'layout', 'presentation', Ini);
  SetIniString('appliance', 'window', '', Ini);
  SetIniString('appliance', 'webrtc_port', '8889', Ini);
  SetIniString('appliance', 'virtual_camera', BoolToStr2(WizardIsComponentSelected('vcam')), Ini);
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
