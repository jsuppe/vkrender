# vkrender sender

A cross-platform **WebRTC/WHIP sender** for vkrender. Capture this device's
screen (or camera) and cast it into a vkrender scene, where it lands as one
more producer stream on the compositor — exactly like OBS's WHIP output or a
browser WHIP sender would. One Flutter codebase targets **Android, iOS,
Windows, macOS, Linux, and web**.

## Why WHIP

Of the four cast paths the vkrender receivers speak, only WebRTC/WHIP (M20) is
senderable from a neutral cross-platform app — Miracast needs a Wi-Fi Direct
source role, AirPlay sending is Apple-proprietary, and Google Cast is
attestation-locked. The receivers run GStreamer `whipserversrc` (a WHIP
*server*); this app is the matching sender, so it required **zero receiver
changes**.

## Architecture

```
 capture (getDisplayMedia / getUserMedia)
        │  MediaStream
        ▼
 RTCPeerConnection ──prefer H.264──► SDP offer
        │                                │  HTTP POST application/sdp
        │                                ▼
        │                   http://<host>:<port>/whip/endpoint  (whipserversrc)
        │                                │  201 Created + answer SDP
        ▼◄───────────────setRemoteDescription───────────────────┘
   SRTP media ──────────────────────────► receiver → vkrender scene
```

- **`lib/whip_client.dart`** — the entire WHIP signalling surface: one POST of
  the offer, the `201` body is the answer, `Location` is the resource to
  `DELETE` on stop. No signalling server.
- **`lib/webrtc_sender.dart`** — capture, `RTCPeerConnection`, and **H.264
  codec-preference forcing**. The Android receiver's pipeline is
  `rtph264depay ! h264parse` (H.264 only), so the sender reorders codecs so
  H.264 is offered first; otherwise a default VP8 negotiation would connect but
  decode nothing there.
- **`lib/discovery.dart`** — mDNS/DNS-SD browse for `_vkrender-whip._tcp`
  (advertised by the receivers). Web has no mDNS, so it falls back to manual
  `host:port` entry.
- **`lib/screen_capture_service.dart`** + `android/.../ScreenCaptureService.kt`
  — the Android `mediaProjection` foreground service that Android 14+ requires
  to be running before `getDisplayMedia`. No-op on every other platform.

## Platform status

| Target | Build here | Screen capture | Notes |
|---|---|---|---|
| Android | ✅ | mediaProjection FGS (wired) | camera also works |
| Windows | ✅ | getDisplayMedia | prebuilt libwebrtc pulled by the plugin |
| Web | ✅ | browser getDisplayMedia | no mDNS → manual entry only |
| Linux | on melchior | getDisplayMedia | |
| iOS | needs a Mac | ReplayKit extension (TODO) | camera works; permissions in Info.plist |
| macOS | needs a Mac | getDisplayMedia | entitlements set |

## Run

```bash
flutter run                 # attached device / default
flutter run -d chrome       # web (manual receiver address)
flutter run -d windows      # desktop
```

Pick a discovered receiver, or type its `host:port` (WHIP port defaults to
8889), choose **Screen** or **Camera**, and cast.
