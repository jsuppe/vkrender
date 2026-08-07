# The vkrender virtual camera — how it registers, and what Teams Rooms wants

The scene-out consumer path (M24) ends in a virtual camera so the composited
presentation becomes a webcam for any conferencing app. This document records
**what the registration actually looks like on Windows**, measured rather than
assumed, and analyses whether it can serve as a Microsoft Teams Rooms
**content camera**.

## Two tiers, one consumer loop

| Tier | Flag | Registers as | Seen by |
|---|---|---|---|
| DirectShow (softcam) | default | `@device_sw_{860BB310-…}\DirectShow Softcam` | DirectShow apps only |
| Media Foundation | `--mf` | `SWD\VCAMDEVAPI\<hash>` | MF **and** DirectShow apps |

Both are fed by the same consumer loop in
[client/vcam_win.cpp](../client/vcam_win.cpp). The MF tier is the one that
matters for modern apps: `MFCreateVirtualCamera` registrations are bridged into
DirectShow automatically, so one registration covers everything. OBS Virtual
Camera and NVIDIA Broadcast still use the legacy DirectShow-only path.

## Measured registration facts (CASPAR, 2026-08-07)

With `vkr_vcam --mf` running:

| Property | Value |
|---|---|
| Instance path | `SWD\VCAMDEVAPI\3B9CC2F8EB65D323146AE6A4A85BAEB8ADC4CB8FBCBC83BF3586AFB96675F5AB` |
| PnP class | `SoftwareDevice` — **not** `Camera` or `Image` |
| PnP friendly name | `Windows Virtual Camera Device` (generic — *not* "vkrender Camera") |
| Device interface class | `KSCATEGORY_VIDEO_CAMERA` `{e5323777-f976-4f5b-9b55-b94699c46e44}` — the same class as a real USB camera |
| Name seen by apps | `vkrender Camera (Windows Virtual Camera)` |
| Stability | **Identical instance path across process restarts** (hash of name + CLSID, not session-random) |

Two traps worth knowing, both of which cost time here:

- **`Get-PnpDevice -Class Camera` does not list it.** The devnode is class
  `SoftwareDevice`. Query by instance path instead.
- **The PnP friendly name is generic.** Every `MFCreateVirtualCamera` camera on
  the machine is called "Windows Virtual Camera Device" at the PnP layer. Only
  the app-facing enumeration carries our name.

### Diagnostics

```powershell
# Is our camera registered right now?
Get-PnpDevice | Where-Object { $_.InstanceId -like "*VCAMDEVAPI*" } |
  Select-Object Status,Class,FriendlyName,InstanceId

# Every camera device interface on the box (real + virtual)
Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Control\DeviceClasses\{e5323777-f976-4f5b-9b55-b94699c46e44}" |
  ForEach-Object { $_.PSChildName }

# What applications actually see
ffmpeg -hide_banner -list_devices true -f dshow -i dummy
```

## Lifetime: why it is System, and what that costs

Originally the camera was created with `MFVirtualCameraLifetime_Session`: it
existed only while `vkr_vcam.exe` ran and vanished on exit. That is wrong for
any consumer that *persists a camera choice* — Teams Rooms stores a content
camera and health-checks it, so a camera that disappears with the appliance
reads as missing hardware.

The camera is now created `MFVirtualCameraLifetime_System` +
`MFVirtualCameraAccess_AllUsers`, so the registration survives process exit and
reboot and the Camera Frame Server activates `vkrender_vcam_source.dll` on
demand.

**The privilege check is on `Start()`, not `Create()`.** Measured unelevated:

```
MFCreateVirtualCamera(System, AllUsers)  -> S_OK
IMFVirtualCamera::Start()                -> 0x80070005  E_ACCESSDENIED
```

So a naive "fall back if create fails" never fires and you silently end up with
no camera at all. `startVirtualCamera()` therefore creates, *tries to start*,
and on failure `Remove()`s the half-registered system camera before falling
back to the session-scoped pair. An unelevated run behaves exactly as it did
before and says so on stdout.

Because a system-lifetime camera outlives uninstall, `vkr_vcam --unregister`
removes it (elevated), and the installer's uninstall step calls it.

## Teams Rooms: can this be a content camera?

MTR-W has **two different content paths with two different gates**, and
conflating them wastes time.

### HDMI ingest — closed to us

> "The Microsoft Teams Room app checks whether one of the HDMI ingest modules
> that's included in the certified Teams Rooms system is detected. It does this
> by using a set of corresponding device peripheral VID/PID (Vendor ID +
> Product ID) as a reference."
> — [MS: HDMI ingest status Unhealthy](https://learn.microsoft.com/en-us/troubleshoot/microsoftteams/teams-rooms-and-devices/hdmi-ingest-status-unhealthy)

A hardcoded VID/PID allowlist. A software camera has no USB VID/PID at all, so
this path is closed by construction. This is also why generic capture cards and
HDMI-over-IP extenders don't work as MTR content sources.

### Content camera — open in principle

`<ContentCameraId>` in `SkypeSettings.xml` takes a **device instance path**,
which MTR uses purely as an identity/persistence token — there is no allowlist
comparison. Microsoft's own guidance is that *any* certified MTR camera works,
with content-camera-certified models merely recommended.

Our instance path is well-formed and stable, so the mechanism fits. Relevant
settings:

```xml
<Devices>
  <ContentCameraId>SWD\VCAMDEVAPI\3B9CC2F8…</ContentCameraId>
  <ContentCameraEnhancement>false</ContentCameraEnhancement>
</Devices>
```

`ContentCameraEnhancement` **must be false** for us. Left on (the default), MTR
detects whiteboard corners, auto-crops, boosts ink lines and makes the person
writing transparent — which would destroy a composited multi-presenter scene.
Microsoft documents `false` as the setting for sending "a raw video feed to
meeting participants… where the camera is used to show sticky notes, posters,
or other media", which is exactly our case.

### What is still unverified

**Whether MTR's peripheral picker enumerates a `SoftwareDevice`-class camera.**
Microsoft's setup instructions say to find the content camera under Device
Manager → *Imaging devices*; ours appears under *Software devices*. If MTR
filters by device class rather than enumerating `KSCATEGORY_VIDEO_CAMERA`, the
path closes. This needs a real MTR box — it is now a five-minute test, not an
open question.

Also note: a camera configured as the content camera **cannot** simultaneously
be the room camera.

## Sharing cannot be toggled programmatically

There is no supported API to start or stop content sharing on MTR:

- The undocumented third-party meeting/call control integration some hardware
  used is **retired 2026-06-30** (MC1266901) — and it only ever covered
  leave/mute/hand/camera/blur/reactions, never content share.
- **Room controls** is an OEM app launcher (lights, blinds, video routing). It
  does not drive share state.
- `<AutoScreenShare>` is admin policy, not a runtime control.
- Content camera sharing is a manual tap on the console.

The only real programmatic lever is **HDMI signal presence** — MTR auto-shares
when a signal appears and stops when it drops. That is precisely what Airtame,
Barco and Crestron exploit, and why Microsoft warns against connecting sources
that always output a signal.

**Consequence for vkrender:** a virtual camera can get the composited scene
*into* the room as content, but a human taps Share once. If automatic start/stop
is a requirement, the lever is an HDMI output feeding the room's own (already
allowlisted) ingest module — not a virtual camera.

## Sources

- [The HDMI ingest status of a Teams Rooms device is Unhealthy](https://learn.microsoft.com/en-us/troubleshoot/microsoftteams/teams-rooms-and-devices/hdmi-ingest-status-unhealthy)
- [Learn how to set up content cameras](https://learn.microsoft.com/en-us/microsoftteams/rooms/content-camera)
- [Remotely manage Teams Rooms device settings (XML config)](https://learn.microsoft.com/en-us/microsoftteams/rooms/xml-config-file)
- [Teams Rooms home screen and admin controls](https://learn.microsoft.com/en-us/microsoftteams/rooms/mtr-home-refresh)
- [Certification specifications for Teams devices](https://learn.microsoft.com/en-us/microsoftteams/devices/certification-specifications)
  — note the **Accessory Signaling Protocol** (the HID-level signalling layer)
  is explicitly OEM-partner-only, which is why no public document describes a
  HID "signal present" message for ingest devices.
