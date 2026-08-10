# Deliverables: two sub-projects, and vkrender as their demonstration

What exists today, what is verified, and what is missing — written to make the
repo split reviewable before any files move.

The proposition is that vkrender stops being the host of this work and becomes
its best example:

1. **framelink** — a cross-platform library for sharing GPU frames between
   processes. Producers and consumers, zero copies, one C API.
2. **the virtual camera** — a library *integration*: it creates an OS camera and
   hands out the buffers another application renders into. "Make your app a
   webcam" without that app knowing anything about cameras.
3. **vkrender** — a compositor that consumes both. Not a dependency of either.

## 1. framelink

| | |
|---|---|
| Code | `framelink/include/framelink.h`, `framelink_d3d11.h`, `framelink/src/framelink_win.cpp` (~380 lines), `fl_consume` / `fl_produce` |
| Links | `d3d11`, `dxgi`, and vkrender's IPC. No Vulkan, no GStreamer, nothing GPL |
| Design | [FRAMELINK.md](FRAMELINK.md) |

**Verified.** 40 frames pixel-verified across two processes, where each frame's
colour is checked against the pts that travelled with it — so slot aliasing and
missing fence waits fail the test, not just "no pixels". Slow-consumer drop path
measured: 57 skipped, 0 mismatched, producer bursting to 27.6 fps against a
20 fps reader. And it has a real dependent in production use (below), which is
better evidence than any test.

**Missing:** Linux (dma-buf) and Android (AHardwareBuffer) backends;
`framelink_vulkan.h`; access control; NV12 and RGB10A2; fan-out to N consumers
(today N consumers means N channels); the optional broker.

## 2. The virtual camera

The framing that matters: **it creates the camera and provides the buffers.** It
owns the framelink ring and registers the OS device; any process calls
`flOpenProducer`, receives GPU buffers, draws into them, submits. vkrender's
renderer is merely the first such application and nothing about it is special.

| | |
|---|---|
| Code | `client/vcam_win.cpp`, vendored MIT `softcam` (DirectShow filter), `vkrender_vcam_source.dll` (Media Foundation source), `vcam_shared.h` |
| Tiers | DirectShow softcam (default) and `--mf` MFCreateVirtualCamera, which Windows also bridges into DirectShow |

**Verified.** An ordinary DirectShow client enumerated `vkrender Camera
(Windows Virtual Camera)`, negotiated `bgr0 1280x720 @30` and pulled 91 frames
in 3 s of real Vulkan-rendered content. Gating confirmed too: with no presenter
the renderer stops blitting and the camera goes idle rather than broadcasting an
empty scene.

**Missing / known:**
- Elevation for a persistent machine-wide camera. Unelevated,
  `MFCreateVirtualCamera(System, AllUsers)` creates but `Start()` returns
  `E_ACCESSDENIED`, so it falls back to session scope and dies with the process.
- Appears to be single-consumer: while the Windows Camera app held the device,
  ffmpeg got zero frames and an I/O error; closing it made capture work at once.
  Not yet investigated.
- No Linux or Android equivalent — see below.

## 3. Extraction plan

The one real entanglement is the transport. `framelink` uses `vkr::IpcChannel`
and `vkr::proto`, which live in vkrender's `common/`. Reusing them was right for
the spike (it put the *proven* transport under test) but a standalone library
cannot depend on a compositor.

- **framelink** → own repo. Vendor a trimmed IPC: `ipc_win32.cpp` plus a
  framelink-owned message set, replacing `proto::ClientHello`/`PoolDesc`/
  `FrameReady`, whose vkrender names would be confusing in a general library.
  MIT or Apache-2.0.
- **virtual camera** → own repo, depending on framelink. Brings `softcam` (MIT)
  and the MF source. Keep it clear of the GPLv3 AirPlay receiver, which is
  already in its own repo.
- **vkrender** → consumes both. `renderer/win_service.cpp` already attaches as
  an ordinary producer; nothing further changes.

Order matters: extract framelink first and let the camera and vkrender build
against it, so the API is exercised by two independent consumers before it is
published.

## 4. Linux: dma-buf backend + v4l2loopback

The Linux virtual camera is the mirror of the Windows one: it creates
`/dev/videoN` through v4l2loopback, owns a framelink ring, and any application
renders into the buffers.

**Surveyed on melchior (Ubuntu 24.04.4), 2026-08-10:**

- `v4l2loopback` **0.15.3 is already installed** (`/lib/modules/7.0.0-28-generic/
  kernel/v4l2loopback/v4l2loopback.ko.zst`), not currently loaded.
- No `/dev/video*` yet; `v4l2-ctl` (v4l-utils) is not installed.
- Kernel headers present; GPU at `/dev/dri/card1` + `renderD128`, so dma-buf
  export is available.
- Toolchain complete: cmake, g++, ninja, pkg-config.
- **No passwordless sudo**, so the device cannot be created from here.

**The one command needed** (creates the loopback device; `exclusive_caps=1`
makes it appear as a capture device, which Chrome and Zoom require):

```bash
sudo modprobe v4l2loopback devices=1 video_nr=10 card_label="vkrender Camera" exclusive_caps=1
```

Making it persistent across reboots is `/etc/modules-load.d/` plus
`/etc/modprobe.d/`, also root.

**Work, in order:**
1. `framelink_linux.cpp` — the same C API over dma-buf fds passed as
   `SCM_RIGHTS` on a unix socket, with `framelink_dmabuf.h` for interop. The
   wire format already carries plane layouts, DRM modifiers and per-plane object
   indices for exactly this.
2. `vcam_linux.cpp` — own a framelink ring, read frames back, `write()` them to
   `/dev/video10` in YUYV or BGR24 (v4l2loopback takes CPU buffers, so unlike
   Windows there is a readback per frame; a dmabuf-import path via
   `VIDIOC_EXPBUF` is a later optimisation).
3. Verify with `ffmpeg -f v4l2 -i /dev/video10`, the mirror of the DirectShow
   capture that verified Windows.

**Honest note on scope:** step 1 is the larger half and is a framelink
deliverable, not a camera one. The camera is comparatively small once frames can
be acquired on Linux.
