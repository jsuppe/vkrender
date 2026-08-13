# The moderator console (M42)

A web page that moderates the room in real time: `https://<appliance>:8443/admin/`.
This is the control plane docs/ARCHITECTURE.md predicted would be the next real
component - "a local HTTP control socket, versioned, with the frame path
untouched" - and that is exactly its shape.

## Architecture

```
browser ── https ──► vkr_web ── Bearer check ── http ──► renderer :8875 (localhost)
  /admin/  page          │                                   │
  /api/*   commands      └── the ONLY off-box path in        ├─ command queue ──► render loop
                             (admin_pin gates it)            └─ published snapshot + preview
```

- The renderer's control API (`renderer/control_api.cpp`, civetweb NO_SSL)
  binds **127.0.0.1 only**. Localhost is its entire security model.
- `vkr_web` reverse-proxies `/api/*` behind `Authorization: Bearer <admin_pin>`
  - the same same-origin pattern that carries WHIP through https.
- HTTP threads never touch Vulkan or the scene: they queue commands and read
  published snapshots. The render loop drains the queue once per frame and
  owns every decision. The live preview follows the same rule - the loop does
  the GPU readback (only while someone is actually watching), HTTP threads
  JPEG-encode from the latest copy.

## Enabling it

```ini
[appliance]
admin_pin=someSecret     ; empty (the default) = console disabled
control_port=8875
```

## Implemented

| feature | notes |
|---|---|
| Live composite preview | ~1.5 s refresh; readback only while a moderator is watching |
| Presenter roster | per slot: resolution, fps, live/stale age, badges |
| Hide / Show | stays connected, removed from the layout; producer keeps flowing |
| Kick | transport-level (close + recreate the slot channel) - works identically for WHIP, AirPlay, Miracast, RTSP, anything |
| Layout switch | presentation / two-cubes / single-cube / wall |
| Spotlight | one presenter fullscreen, everything else waits |
| Pin | holds a presenter on screen through the rotation |
| Move to front | reorders the seating |
| **Max visible tiles** | cap the layout at N (1-12) |
| **Carousel rotation** | more presenters than seats → the overflow rotates through the spare seats every `rotate_seconds`, pinned ones stay; transitions use the presentation layout's own enter/exit animation |
| Pause / resume | freezes the scene clock |
| Moderator auth | Bearer token (`admin_pin`), enforced at the web tier |

Under the hood the selection rule is: spotlight wins outright; otherwise pinned
presenters take seats first, and the rest carousel through what remains. Hidden
and rotated-out producers keep their consumed-fences signalled so they never
stall behind a moderation decision they cannot see.

## Roadmap (in feature-list order, honestly not built yet)

- **Audio: mute / per-presenter volume.** The entire pipeline is VIDEO-ONLY
  today - AirPlay audio is discarded, WHIP audio tracks are ignored, and the
  virtual camera has no microphone half. Audio ingestion (+ a virtual mic
  sink) is its own milestone; mute buttons appear when there is audio to mute,
  not before.
- **PIN display / rotation from the console.** Receivers read VKR_PIN at
  startup; rotating it live means teaching them a control channel first.
- **Presenter names.** Slots are `in.N`; WHIP/AirPlay session names could be
  attached to slots via receiver-side reporting.
- **Per-tile preview thumbnails** (per-stream readback, not just the composite).
- **Event log / audit trail** of moderator actions.
- **Layout editor** (custom tile regions, not just the presets).
- **Kick-with-ban** (kick today is transport-level; the presenter's app may
  simply reconnect - a ban list needs receiver cooperation).

## Two bugs this feature forced into the open

- **framelink (Windows): `flClose` hung when the producer was still live.**
  The close-poke only unblocks an accept thread waiting in ConnectNamedPipe; a
  pump attached to a streaming producer sits in recv(), fed forever at 30 fps,
  so join() blocked the render thread solid. Kick was the first consumer-side
  close with a producer mid-stream in the codebase's history. Fixed with
  CancelSynchronousIo, retried until the thread exits.
- **vkr_web: dangling response pointers in the proxy.** `mg_get_response_info`
  returns pointers into the connection's receive buffer, and reading a large
  body (the preview JPEG) recycles that buffer. Status text and header
  pointers must be copied BEFORE the body read; crashing intermittently under
  the console's polling is exactly how it announced itself.
