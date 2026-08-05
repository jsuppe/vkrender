# Roadmap

## Done — M1–M4: Windows pipeline (2026-08)

- Two-process zero-copy pipeline: RTSP → D3D11VA decode → shared BGRA texture
  ring → Vulkan renderer. Verified end-to-end at 1080p30.
- Shared D3D11 fences imported as Vulkan timeline semaphores; latest-frame-wins
  low-latency design (`--pool-size`, MAILBOX present, FFmpeg no-buffer flags).
- Platform-neutral IPC protocol already carrying dma-buf plane-layout fields.
- Finding: NV12 D3D11→Vulkan import is broken on current NVIDIA drivers
  (chroma plane offset mismatch → device lost); Windows shares BGRA via
  VideoProcessor instead. NV12 renderer path retained for dma-buf platforms.

## M5 — Windows hardening (2026-08)

- [x] Renderer reconnect loop: redials the pipe once a second after a decoder
      death, resends `Hello`, reimports on the next `PoolDesc`; decoder runs a
      session loop and serves renderer restarts the same way.
- [x] Decoder RTSP auto-reconnect with capped exponential backoff (1→10 s),
      5 s socket I/O timeout so stalls become errors, `StreamStatus` protocol
      message surfaced in the renderer window title. Verified: publisher kill /
      republish, decoder kill / restart, renderer kill / restart.
- [x] PTS-based presentation pacing (`vkrenderer --pacing pts`): frames queue
      until their anchored presentation time; re-anchors on >1 s discontinuity;
      force-drains so the queue never starves the decoder's slot ring.
      Default remains latest-frame-wins (lowest latency).
- [x] H.265 verified end-to-end (8-bit and 10-bit). 10-bit P010 sources get an
      RGB10A2 shared pool (VideoProcessor output support permitting) imported
      as `VK_FORMAT_A2B10G10R10_UNORM_PACK32`; pool rebuilds automatically on
      bit-depth changes.
- [~] AV1: the decode path is codec-agnostic and AV1 D3D11VA is expected to
      work on this GPU, but FFmpeg's RTSP muxer cannot publish AV1, so no
      local end-to-end test exists yet — needs a GStreamer or WebRTC/SRT
      publisher to verify.
- [x] CI: GitHub Actions Windows build with FFmpeg download cache and
      binary artifact upload.

## M6 — Linux backend (VAAPI + dma-buf) — DONE (2026-08-03)

Same two binaries on Linux with **blit-free** zero-copy: the renderer samples
the decoder's own decode surfaces (a ring of held `AVFrame` refs — no
conversion pass, no in-process copy at all). Verified end-to-end on real
hardware: RTX 3090 / driver 580.173 / Ubuntu 24.04 / nvidia-vaapi-driver,
H.264 1080p30, headless render with pixel-correct frame dumps. Full details,
setup and troubleshooting: [docs/LINUX.md](docs/LINUX.md).

- [x] `common/ipc_posix.cpp`: SOCK_SEQPACKET Unix socket + SCM_RIGHTS fd
      passing; `vkr_ipc_test` self-test runs in CI and on dev boxes.
- [x] `decoder/vaapi_decoder.cpp`: FFmpeg VAAPI decode, per-surface
      `vaExportSurfaceHandle(DRM_PRIME_2, SEPARATE_LAYERS)` announced once as
      `SurfaceDesc` (+fds), held-frame ring with FrameConsumed-driven release,
      `vaSyncSurface` producer sync, RTSP auto-reconnect shared with Windows.
- [x] Renderer dma-buf import with explicit DRM-modifier plane layouts; three
      topologies: single-object combined, multi-object DISJOINT combined, and
      **planar R8/R8G8 fallback** (auto-selected when the driver cannot sample
      the exported modifier as multi-planar NV12 — the NVIDIA case) with
      shader-side YUV→RGB.
- [x] `--headless N --dump out.ppm` renderer mode (no window system): renders
      offscreen and writes a verification frame — how the SSH-only reference
      box is tested.
- [x] CI: ubuntu-latest build + IPC test job.
- Backlog (M6.1, tracked as GitHub issues): [#1 sync_file explicit
      sync](https://github.com/jsuppe/vkrender/issues/1), [#2 Intel/AMD
      combined-mode validation](https://github.com/jsuppe/vkrender/issues/2),
      [#3 Linux 10-bit P010](https://github.com/jsuppe/vkrender/issues/3),
      [#4 hybrid-GPU](https://github.com/jsuppe/vkrender/issues/4).

## M7 — Android backend (MediaCodec + AHardwareBuffer) — DONE (2026-08-03)

One APK, two OS processes: `DecoderService` (`android:process=":decoder"`,
FFmpeg RTSP demux + `AMediaCodec` + `AImageReader`) shares each decoded
frame's `AHardwareBuffer` over the abstract Unix socket
(`sendHandleToUnixSocket`); the `NativeActivity` renderer imports them via
`VK_ANDROID_external_memory_android_hardware_buffer` + external-format YCbCr
conversion. Zero copies, zero conversions; held-image ring + FrameConsumed
release, same protocol as desktop. Full details: [docs/ANDROID.md](docs/ANDROID.md).

Verified end-to-end on the emulator (API 36 x86_64, gfxstream on host RTX
5090): 720p30 H.264 RTSP from the host → animating video with 6 buffers
cycling zero-copy. Two emulator-specific caveats, both documented: the
goldfish hardware-codec shim stalls (auto-fallback to the software codec on
`ro.boot.qemu`), and gfxstream mis-routes chroma for YV12 external formats
(colors shift; structure/animation correct).

M7.1 hardening (2026-08-03):
- [x] Decoder runs as a **foreground service** (`dataSync` type) — survives
      screen-off; verified decoding continuously through a 90 s sleep cycle.
- [x] Freeze/thaw watchdog: wall-clock deadline via `AVIOInterruptCB` aborts
      stale demux reads after a thaw and drops into the normal reconnect path.
- [x] Producer sync upgraded to **acquire fence fds**
      (`AImageReader_acquireNextImageAsync` → fence over SCM_RIGHTS with
      `FrameReady` → renderer waits via pollable sync_file, no Vulkan
      extension dependency, no decoder-side CPU wait).
- [x] Renderer lifecycle: IPC pump continues while paused (frames are
      consumed-and-discarded so the decoder ring never starves), and the
      surface/swapchain rebuild on window replacement — verified across
      sleep/wake.
- [x] CI `android` job: NDK + cached FFmpeg static build + Gradle, APK artifact.
- [x] **Real-device verification** ([#5](https://github.com/jsuppe/vkrender/issues/5),
      closed): TCL 8483A tablet, Android 15, arm64, vendor hardware codec,
      vendor-private external format import, **correct colors** (confirming
      the emulator palette shift is gfxstream-only), smooth 30 fps after two
      device-informed fixes: IDENTITY swapchain preTransform (display was
      180° rotated) and non-blocking fence adoption + 8-deep held ring
      (hardware acquire fences lag the output callback; the 4-deep ring
      dropped ~10 fps).

## M8 — Android: renderer-owned Surface over Binder — DONE (2026-08-03)

Architecture inversion proposed by Jon: the renderer owns the `AImageReader`
and hands its `Surface` to producers through an AIDL service; MediaCodec
decodes directly into it and BufferQueue replaces the entire custom frame
plane (socket AHB passing, fence fds, consumed handshake, held ring, pending
window — all deleted). Verified same-day on the TCL 8483A and the emulator:
identical telemetry to the tuned M7.1 pipeline (60 Hz vsync-locked presents,
exact 30 fps adoptions, buffers cycling zero-copy) with ~500 fewer lines of
platform code. The renderer is now effectively a compositor service — any
Surface producer (camera, other apps with permission gating) can feed it.
Details: [docs/ANDROID.md](docs/ANDROID.md). Closed
[#7](https://github.com/jsuppe/vkrender/issues/7).

## M9 — GStreamer decode pipelines (done, 2026-08-03)

Replacing FFmpeg with GStreamer end-to-end (motivation: team familiarity).

- [x] **Linux**: `rtspsrc → decodebin3 → vah264/265dec`, appsink pinned to
      `memory:VAMemory`, per-buffer `VASurfaceID` via gstreamer-va-1.0 +
      the M6-verified dma-buf export. Zero-copy negotiates on Mesa/iHD
      (Intel/AMD); the NVIDIA shim only offers sysmem caps (decode proven on
      melchior, zero-copy needs Intel/AMD hardware). Hardware-agnostic
      fallback for VMs tracked in [#8](https://github.com/jsuppe/vkrender/issues/8).
- [x] **Windows**: `rtspsrc → decodebin3 → d3d11 decoders` on our own D3D11
      device (GstContext sharing), appsink `memory:D3D11Memory` (NV12/P010),
      unchanged VideoProcessor→shared-pool→fence→IPC path. **Verified
      end-to-end on the RTX 5090**: 30 fps exact, correct colors. Requires
      the GStreamer MSVC runtime+devel MSIs (CMake reports clearly; CI
      installs silently).
- [x] **Android**: transport/demux swapped to static GStreamer prebuilts
      (`rtspsrc → parsebin` bridged through an outer `h26xparse` to an
      au-aligned appsink — parsebin's internal capsfilter pins its exposed
      pad to `alignment=nal`, so a direct link to an au-only appsink fails
      `not-linked`) feeding the M8 MediaCodec-into-renderer-Surface path
      (decode element stays platform MediaCodec: `amcvideodec` cannot target
      a foreign AImageReader surface without an extra GL copy). **Verified on
      the TCL tablet (hardware codec, ~150 adoptions/5 s = exact 30 fps,
      13-buffer zero-copy pool) and the API 36 emulator** (software-codec
      fallback). CI builds the APK against a pruned ~130 MB/ABI prebuilt
      cache ([scripts/fetch_gstreamer_android.sh](scripts/fetch_gstreamer_android.sh)).
- FFmpeg now remains only as the desktop test-stream publisher CLI (dev
  tooling, not product).

## M10 — Android two-APK split + spinning-cube scene (done, 2026-08-04)

The M8 Binder seam promoted to a real app boundary (primary deployment
target is Android):

- **Renderer service APK** (`dev.vkrender.renderer`): NativeActivity +
  exported `RendererService` behind a signature-level `BIND_RENDERER`
  permission. Vulkan only — no GStreamer. Renders the video zero-copy onto
  all six faces of a spinning cube (`cube.vert` MVP push constant,
  back-face culling instead of a depth buffer; winding verified on device —
  mirrored timestamp text is the inside-out tell).
- **Decoder client APK** (`dev.vkrender.decoder`): binds the renderer app
  by explicit component (+ `<queries>` for package visibility), receives
  the `Surface` over AIDL, decodes the RTSP stream into it. GStreamer +
  MediaCodec only — no Vulkan.
- Lifecycle hardening found during verification: MediaCodec feeding an
  abandoned Surface stalls **silently** when the renderer app dies; the
  kept-alive Binder connection's `onServiceDisconnected` now aborts the
  native session (~75 ms) and the client re-binds (verified: kill renderer
  → relaunch → full-rate again < 10 s).
- Verified on the TCL tablet (~150 adoptions/5 s = exact 30 fps, 13-buffer
  zero-copy pool, hardware codec) and the API 36 emulator. CI builds both
  APKs.

## M11 — Multi-producer compositing (done, 2026-08-04)

The renderer service becomes a real compositor: each client app (Binder
UID) gets its own `AImageReader`, up to six streams — one per cube face
(fewer streams cycle across faces; `recordCube` binds a descriptor set per
face; barriers cover the unique slots).

- Client packages need zero code changes: same AIDL, same
  `acquireInputSurface()` — the per-UID keying is server-side. Six decoder
  flavors (`dev.vkrender.decoder` + `decoder2..6`) build from one codebase
  to demo a full cube: **verified all six faces live simultaneously** on the
  TCL tablet (testsrc2, SMPTE bars, mandelbrot, gradients, rgbtestsrc,
  testsrc — ~870 adoptions/5 s ≈ 6 × 30 fps, 101 imported slots, presents
  locked at 60 Hz; `kMaxSurfaces` raised to 128 to hold six hardware codec
  buffer pools).
- Stream lifecycle: client death freezes its face on the last frame (other
  streams unaffected); on restart the client releases-then-reacquires so
  the renderer frees the stale stream's imported slots (bounded pool across
  restarts — without it each codec restart leaked ~14 slot imports toward
  the `kMaxSurfaces=64` cap). `ExternalVideoPool::removeSlot` +
  freeable descriptor sets make per-stream teardown possible.
- Verified on the TCL tablet: two client APKs streaming simultaneously
  (testsrc2 + SMPTE bars on adjacent faces), 300 adoptions/5 s = 2 × 30 fps
  exact, 27-28 slot imports stable across client restarts.

## M12 — Dual cubes, 12 streams, vaporwave scene (done, 2026-08-04)

Scaled the compositor to **twelve simultaneous client apps** — two spinning
cubes stacked vertically (counter-rotating, both fully in frame), one
stream per face — over a procedural vaporwave backdrop
([vaporwave.frag](renderer/shaders/vaporwave.frag): violet-to-orange sunset
sky, striped synthwave sun on the horizon, neon perspective grid scrolling
toward the camera; drawn by the new `BackgroundPass` inside the same render
pass, before the cubes).

- `VideoPass::recordScene` generalizes the cube path to N cubes (per-cube
  MVP push constants, per-face descriptor sets, barriers over unique slots,
  single render pass).
- `kMaxSurfaces` 128 → 256: twelve concurrent codec sessions imported ~200
  slots on the TCL tablet.
- Codec hardening: hardware decoder first, **software fallback** when
  create/configure/start fails — vendors cap concurrent hardware sessions
  and twelve at once can exceed the limit.
- Twelve decoder flavors (`decoder` + `decoder2..12`). Verified on the TCL
  tablet: 12 distinct patterns live (testsrc2, SMPTE HD/classic bars,
  mandelbrot, gradients, rgbtestsrc, testsrc, sierpinski, game-of-life,
  cellauto, yuvtestsrc, colorspectrum), ~1350 adoptions/5 s aggregate,
  presents locked at 60 Hz.

## M13 — Wayland compositor front door (done, 2026-08-04)

The Linux equivalent of the Android Binder/BufferQueue architecture:
`vkrenderer --wayland [socket]` runs a **minimal Wayland compositor**
([renderer/wayland_compositor.cpp](renderer/wayland_compositor.cpp)) —
ordinary Wayland clients are producers with **zero custom code**:

```bash
vkrenderer --wayland vkrender-0 &
WAYLAND_DISPLAY=vkrender-0 gst-launch-1.0 rtspsrc location=rtsp://... ! decodebin3 ! waylandsink
```

- Protocol surface: `wl_compositor`, `wl_subcompositor` (waylandsink parks
  video on a subsurface), `wl_shm`, xdg-shell (configure dance), and
  `zwp_linux_dmabuf_v1` v3 with format/modifier advertisement from the
  Vulkan driver's `VK_EXT_image_drm_format_modifier` query (vendored
  protocol XMLs + build-time wayland-scanner codegen).
- Buffers: dma-bufs import zero-copy through the M6 `SurfaceDesc` path
  (implicit-sync bridged via `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` polling,
  mirroring Android's fence handling); `wl_shm` buffers take a CPU-upload
  path into renderer-owned linear images — which is also the
  hardware-agnostic VM answer from [#8](https://github.com/jsuppe/vkrender/issues/8).
  `wl_buffer.release` is deferred behind frame fences exactly like
  Android's `AImage` retirement; frame callbacks pace clients at composite
  rate.
- Same scene as Android: streams land on the two-cube vaporwave scene
  (side-by-side in landscape), up to 12.
- **Verified on melchior** (RTX 3090, headless over SSH): stock
  `videotestsrc ! waylandsink`, three simultaneous clients, and full
  RTSP → `decodebin3` → `waylandsink` at exact 30 fps. The udmabuf test
  client ([test/wayland_dmabuf_client.c](test/wayland_dmabuf_client.c))
  verified the dmabuf protocol path end-to-end; NVIDIA declines to import
  system-RAM dma-bufs (clean failure + release), so dmabuf zero-copy lands
  with Intel/AMD hardware ([#2](https://github.com/jsuppe/vkrender/issues/2)).

## M14 — Windows client library + service mode (done, 2026-08-04)

Windows has no OS producer protocol (no Binder, no Wayland), so we ship the
"Surface" ourselves — completing the client-of-a-service model on all three
platforms:

- **`vkrenderer --serve [pipe]`**: the renderer owns each client's buffer
  ring (mirroring `AImageReader`). A D3D11 allocator device creates shared
  NT-handle BGRA textures + a shared ready/consumed fence pair per client
  (the M2 machinery, ownership inverted), imports them into Vulkan through
  the proven M3 paths (`addSurfaceWin32` dynamic import, header-only pool),
  duplicates the handles into the client process, and composites client
  streams on the two-cube vaporwave scene. Sync is fully GPU-side: submit
  waits each client's readySem at its frame value and signals consumedSem
  per the M2 contract.
- **`vkrender_client.dll`** ([client/vkrender_client.h](client/vkrender_client.h)):
  a four-call C API — `vkrConnect` (pass your D3D11 device or let the
  library create one on the renderer's adapter), `vkrDequeueBuffer`
  (backpressure via a GPU-driven consumed-fence wait), `vkrQueueBuffer`
  (fence signal + flush + FrameReady), `vkrDisconnect`. BufferQueue
  semantics on Windows in ~300 lines.
- Named pipe listener fixed to `PIPE_UNLIMITED_INSTANCES` so the accept
  loop serves N clients concurrently.
- **Verified on the RTX 5090**: four demo producers
  ([test/win_client_demo.cpp](test/win_client_demo.cpp), each with its own
  D3D11 device and animated pattern) at ~569 adoptions/5 s aggregate,
  16 imported slots, clean per-client teardown.

## M15 — Multi-window spanning, layouts, runtime controls (done, 2026-08-05)

The renderer grows a proper front-of-house
([renderer/scene_host.cpp](renderer/scene_host.cpp), shared by `--serve`
and `--wayland`):

- **Multi-window spanning**: repeatable `--window WxH+X+Y` places windows
  on the desktop AND defines their region of a shared virtual canvas; each
  window renders the scene through an off-center sub-frustum
  (`matFrustumSub`) and a canvas-mapped backdrop, so windows arranged
  across displays show one seamless scene (verified: sun/grid continuous
  through the seam of two 960x540 windows). One submit + one present cover
  all swapchains; scene windows use FIFO (vsync-paced).
- **Layouts** (L key cycles): two-cubes, single-cube, and a flat **wall**
  of 16:9 tiles - one stream each, grid auto-fit to the canvas aspect
  (reuses the cube pipeline drawing only front faces).
- **Controls**: SPACE pause, -/= speed, B backdrop toggle, **1-9 remove/
  restore stream N from the scene** (0 = all back; producers keep flowing,
  they just leave the composition), P snapshot, ESC quit.
- **Snapshots**: P (or `VKR_SNAPSHOT_SECONDS=N`) dumps each window's last
  presented frame to `scene-w<N>.ppm` - swapchains now carry TRANSFER_SRC
  for this, which is also how multi-window output is verified (GDI/
  PrintWindow cannot capture Vulkan flip-model surfaces).
- Verified on the RTX 5090: two spanning windows, six producer clients,
  layout switching and per-stream removal live via keyboard, dynamic
  client connect/disconnect reflected in the scene.

## Later / ideas

- Audio: decode + share via existing IPC, or local playback in the decoder.
- Multiple streams → one renderer (N pipes, N pools, scene composition).
- Vulkan Video decode backend (`VK_KHR_video_decode_*` via FFmpeg Vulkan
  hwaccel) as a single-API cross-platform decoder, once driver support and
  the FFmpeg integration mature — would collapse the per-OS decoder backends.
- HDR: P010/HDR10 end-to-end (VideoProcessor → RGB10A2 on Windows, native
  P010 dma-buf on Linux) + `VK_EXT_hdr_metadata`.
