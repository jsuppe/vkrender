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
| **Camera toggle** (M43) | gates the vcam feed; the meeting sees framecam's idle card, never a frozen frame |
| **Display outputs** (M43) | every monitor listed with geometry + primary badge; Present/Stop attaches a borderless window at runtime |
| **Multi-display spanning** (M43) | active monitors' desktop rects form a union; the canvas aspect-fits inside it, so a wall spans one scene and each panel renders its slice (off-center frusta - the M15 math) |
| **Per-display previews** (M43) | each display card crops the composite preview to that display's canvas rect - which IS what the panel shows; inactive displays show the composite dimmed ("what it would show") |

### M43 display mechanics, honestly

- The headless appliance now initializes the window system at startup (no
  window is created); if there is no display session at all, `displaysAvailable`
  is false and the panel hides. `VkContext::canDisplay` is the gate.
- A display wider than the 16:9 canvas (one 32:9 panel, say) gets canvas
  coordinates beyond [0,1]; the off-center frustum renders that as MORE
  SCENE - the vaporwave backdrop extends - never a crop or a stretch. The
  web preview approximates those side regions as black.
- With a display attached, FIFO present paces the render loop at the panel's
  refresh (the 16 ms headless sleep steps aside), and the display-awake hold
  + wake nudge apply while presenting - a sleeping panel would drag the whole
  scene to ~7 fps (the M28 measurement).
- Closing a display window at the desk (alt-F4) detaches that display; it
  never quits the appliance.

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

## Bugs the features forced into the open

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
- **vkr_web (M43): the OPENSSL_APPLINK abort in `generatePem`.** Passing a
  `FILE*` from the exe's CRT into the OpenSSL DLL (`PEM_write_X509`) killed
  the process silently, leaving a 0-byte pem. Fixed by writing through
  `BIO_new_file`, which opens the file inside OpenSSL's own CRT - no applink
  shim involved.
