# vkrender presentation appliance — installer

Builds `vkrender-setup-<version>.exe`: the renderer, every receiver, and the
virtual camera, packaged as a turn-key presentation appliance.

## Build

```bash
cmake --build build --config Release --parallel
"%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" installer\vkrender.iss
```

The AirPlay receiver comes from the sibling
[vkrender-airplay](https://github.com/jsuppe/vkrender-airplay) checkout
(`..\vkrender-airplay\build\Release\vkr_airplay.exe`); its file entry is
`skipifsourcedoesntexist`, so the installer still builds without it.

## What it installs

| Component | Contents |
|---|---|
| Renderer + launcher (always) | `vkrenderer.exe`, `vkr_appliance.exe`, `vkrender_client.dll`, shaders |
| WebRTC / WHIP receiver | `vkr_webrtc.exe` — Flutter sender, browsers, OBS |
| Miracast receiver | `vkr_miracast.exe` — Windows / Android senders |
| AirPlay receiver | `vkr_airplay.exe` — Apple senders (GPLv3) |
| Virtual camera | `vkr_vcam.exe` + `softcam.dll` (DirectShow) and/or `vkrender_vcam_source.dll` (Media Foundation) — **both registered by the installer** (`regserver`), no manual `regsvr32` |

The wizard asks about PIN policy (off / new code per connection / fixed) and
writes `vkrender.ini` next to the executables.

## Runtime dependencies

**GStreamer MSVC x86_64 runtime** (<https://gstreamer.freedesktop.org/download/>)
— needed by the WebRTC and AirPlay receivers. The installer checks and warns;
everything else works without it.

**Windows "Wireless display" optional feature** — needed to *receive*
Miracast (it supplies the sink infrastructure; see
[docs/MIRACAST.md](../docs/MIRACAST.md)). When you select the Miracast
component and the feature is absent, the wizard offers a checkbox to install
it. It is **offered, never forced**: it is an OS feature that downloads from
Microsoft, can be blocked on managed machines, and also switches on
"Projecting to this PC". If the install fails the wizard says so and
continues — AirPlay and WebRTC are unaffected. Manual equivalent:

```powershell
Add-WindowsCapability -Online -Name App.WirelessDisplay.Connect~~~~0.0.1.0
```

## Virtual camera registration

The Media Foundation camera registers **system-wide** so it stays visible when
the appliance isn't running — required by anything that persists a camera
choice, such as a Teams Rooms content camera. That needs elevation: run
unelevated and it falls back to a session-scoped camera that disappears with
the process (it says which on stdout). The uninstaller runs
`vkr_vcam --unregister` so no ghost camera is left behind.

Details, measured instance paths, and the Teams Rooms analysis:
[docs/VIRTUAL_CAMERA.md](../docs/VIRTUAL_CAMERA.md).

## vkrender.ini

```ini
[appliance]
name=vkrender          ; advertised name on the network
pin=auto               ; auto | <digits> | off
layout=presentation    ; presentation | two-cubes | single-cube | wall
window=                ; empty = 1280x720+80+80; "WxH+X+Y;WxH+X+Y" spans displays
webrtc_port=8889
virtual_camera=1
vcam_mode=dshow        ; dshow | mf
airplay=1
miracast=1
```

`vkr_appliance.exe` starts everything as one kill-on-close group: closing the
launcher (or the scene window) stops every receiver, so nothing is orphaned.
Scene keys still work: `L` cycles layouts, `1`-`9` toggle streams, `ESC` quits.
