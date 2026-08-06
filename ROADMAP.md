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

## M16 — Miracast receiver (Windows) (built, 2026-08-05; live-cast test pending)

`vkr_miracast [pipe] [name]`
([client/miracast_receiver.cpp](client/miracast_receiver.cpp)): the machine
becomes a discoverable Miracast sink ("vkrender") via the public
`Windows.Media.Miracast.MiracastReceiver` API, and the cast is **just
another producer**: `MediaSourceCreated` → `MediaPlayer` in frame-server
mode → `CopyFrameToVideoSurface` into dequeued `vkrender_client` ring
textures → one more stream on the scene, curated like any other (M15
controls apply).

- Requirements: Windows 1809+, and Windows' own "Projecting to this PC"
  feature must not claim the sink role. Phone/tablet senders need a
  Miracast-capable Wi-Fi adapter on the receiver (Wi-Fi Direct); Windows
  senders on the same LAN can connect over infrastructure (MS-MICE).
  Cast audio is discarded (video-only scene).
- Status: builds + session starts + service ring connected on the dev box
  (Ethernet-only, so Wi-Fi Direct discovery is unavailable there);
  end-to-end cast pending a sender device on suitable hardware.
- Linux/Android receiver stacks (Wi-Fi Direct + WFD RTSP, no public sink
  API) stay on the backlog.

## M17 — AirPlay receiver via UxPlay (Linux) (done, 2026-08-05; live cast pending sender)

The M13 compositor makes AirPlay nearly free: **UxPlay** (open-source
AirPlay mirroring server, Ubuntu-packaged) renders through GStreamer, so
pointing its sink at our socket turns every AirPlay mirror into a vkrender
stream with zero integration code:

```bash
vkrenderer --wayland vkrender-0 --headless -1 &   # endless headless service
WAYLAND_DISPLAY=vkrender-0 uxplay -n vkrender -nh -vs waylandsink
```

- New `--headless -1`: endless headless service mode (SSH-only receiver
  boxes) with periodic scene dumps via `VKR_SNAPSHOT_SECONDS` →
  `scene-headless.ppm`.
- Verified on melchior: uxplay attaches to the compositor and the machine
  advertises `vkrender` on `_airplay._tcp`/`_raop._tcp` (confirmed by raw
  mDNS query; avahi running). Apple senders on the LAN list it under
  Screen Mirroring; the mirror lands as a scene stream (shm path on the
  NVIDIA box). End-to-end video pending an Apple sender device.
- Windows AirPlay would need a UxPlay Windows build + a GStreamer sink
  bridging to vkrender_client - backlog.

## M18 — AirPlay receiver on the Android renderer (built, 2026-08-05; live cast pending)

A third producer APK (`dev.vkrender.airplay`) makes the Android tablet an
**AirPlay screen-mirroring sink** - so iPhones/iPads/Macs mirror to the
cube. No attestation wall (unlike Google Cast); the phone sees "vkrender"
in Screen Mirroring and the mirror becomes a scene stream.

- The UxPlay/RPiPlay RAOP + mirroring core (`third_party/uxplay/lib`, GPLv3)
  is compiled for the NDK; its `video_process` callback (H.264 access units,
  SPS/PPS in-band) feeds `AMediaCodec` decoding into the renderer's Binder
  Surface - the exact decode tail as the M9 decoder, minus the transport.
  libplist and the FairPlay/pairing crypto link against the OpenSSL static
  libs already in the GStreamer Android prebuilts.
- Discovery: an Android `dnssd` shim
  ([android/airplay/.../dnssd_android.c](android/airplay/src/main/cpp/dnssd_android.c))
  builds the standard `_raop._tcp`/`_airplay._tcp` TXT records but delegates
  registration to Java's **NsdManager** (no bundled mDNS daemon).
- Lifecycle mirrors the decoder client: binds `RendererService` for a
  Surface, one AirPlay session at a time, foreground service.
- **Verified on the API-36 emulator**: native init, raop server bind, and
  both NSD services register (`A2B5E5BA1D33@vkrender` on `_raop._tcp`,
  `vkrender` on `_airplay._tcp`). Live iPhone → tablet mirroring pending a
  device on a multicast-friendly Wi-Fi network (the emulator NAT can't
  reach a real sender).
- **License note**: the AirPlay APK is GPLv3 (vendored UxPlay core); the
  other vkrender APKs are unaffected (separate process over Binder - the
  M10 split contains the obligation to this one app).

## M19 — AirPlay receiver on Windows (native producer) (built, 2026-08-05; live cast pending)

`vkr_airplay.exe` makes a Windows box an AirPlay screen-mirroring sink,
feeding the cast into the scene as a producer stream - the same vendored
UxPlay core as Android (M18), ported to **MSVC**:

- **Win32 pthread/unistd shim** ([third_party/win_compat](third_party/win_compat)):
  the UxPlay core is written for pthread; a ~150-line shim maps it onto
  CRITICAL_SECTION / CONDITION_VARIABLE / CreateThread, with **lazy mutex
  init** so a zero-initialized handle (POSIX `PTHREAD_MUTEX_INITIALIZER`
  semantics, which `httpd_t`'s `run_mutex` relies on) is still lockable.
- **Native discovery**: `client/dnssd_win.c` registers `_raop._tcp` /
  `_airplay._tcp` through Win32 `DnsServiceRegister` (dnsapi) - **no Bonjour
  install**. Confirmed advertising on the LAN (avahi-browse from another
  host sees the machine's `vkrender` service).
- **Decode**: GStreamer `appsrc -> h264parse -> avdec_h264 -> BGRA appsink`
  (software; the d3d11 hardware decoder crashed during state change on this
  stack), each frame `UpdateSubresource`'d into a dequeued `vkrender_client`
  ring texture. OpenSSL for the FairPlay/pairing crypto comes from the
  GStreamer MSVC install.
- Portability fixes to the vendored deps (all guarded, Android/Linux
  unaffected): libplist GNU statement-expression macros (`get_unaligned`,
  `UINT_TO_HOST`), MSVC `snprintf`/`SOL_TCP`, and `&(size_t){N}` compound
  literals in crypto.c that MSVC miscompiled (the ed25519/x25519 key-length
  out-params) - these caused a real heap/critical-section crash chain that
  took the receiver down until fixed.
- Status: runs, advertises, connects to the service ring; live iPhone cast
  end-to-end pending an Apple sender. GPLv3 (vendored UxPlay core).

## M20 — WebRTC (WHIP) receiver on all three platforms (Windows+Linux verified, 2026-08-05; Android built)

The clearest *shared-implementation* win of the cast family: **one WebRTC
stack — GStreamer `webrtcbin` plus the gst-plugins-rs `whipserversrc` — on
all three platforms**, so a browser (or OBS, or any WHIP sender) can push
its camera/screen into the scene as a producer. No per-OS receiver logic:
the *same* GStreamer pipeline shape runs everywhere; only the terminal sink
differs, matching each platform's existing producer front door.

- **Signaling**: WHIP (WebRTC-HTTP Ingestion Protocol) — a single HTTP POST
  of the SDP offer. `whipserversrc` runs the HTTP endpoint in-process
  (`signaller::host-addr=http://0.0.0.0:PORT`, endpoint path `/whip/endpoint`);
  no external signaling server. ICE/DTLS/SRTP via libnice + GStreamer's
  dtls/srtp plugins. gst-plugins-rs is MPL-2.0 — permissive, unlike the
  AirPlay APK's GPLv3.
- **Windows** ([client/webrtc_win.cpp](client/webrtc_win.cpp), `vkr_webrtc.exe`) —
  **verified end-to-end**: `whipserversrc ! decodebin ! videoconvert !
  appsink(BGRA)`, each frame `UpdateSubresource`'d into a dequeued
  `vkrender_client` ring texture (same producer path as Miracast/AirPlay).
  Keys the video pad off its pad *name* (`video…`) because the caps `media`
  field is still NULL at `pad-added`. 300+ frames of an SMPTE WHIP sender
  landed on the cubes.
- **Linux** — **verified end-to-end**: the receiver is literally one
  gst-launch line — `whipserversrc ! decodebin ! videoconvert ! waylandsink`
  — into the M13 Wayland compositor, exactly like AirPlay via UxPlay. (The
  Ubuntu GStreamer packages don't ship `whipserversrc`; built
  `gst-plugin-webrtc` from gst-plugins-rs with `cargo build --release` and
  dropped `libgstrswebrtc.so` into the user plugin dir.)
- **Android** ([android/webrtc](android/webrtc), `dev.vkrender.webrtc`) —
  **APK builds with the full WebRTC static-link closure** (webrtcbin, nice,
  dtls, srtp, rtpmanagerbad, and the Rust rswebrtc/rsrtp/webrtchttp plugins
  all link and `GST_PLUGIN_STATIC_REGISTER` cleanly for both arm64 and
  x86_64). Pipeline `whipserversrc ! rtph264depay ! h264parse ! appsink`
  feeds H.264 access units to the same `AirplayVideoSink` (M18) →
  `AMediaCodec` → renderer Binder Surface. Startup SIGSEGV on the x86_64
  emulator is under investigation (the emulator has been unrepresentative
  throughout this project — gfxstream color, codec stalls); real-device
  verification pending, consistent with how M16–M19 shipped.

Design payoff: adding WebRTC touched **zero** of the renderer/compositor/
producer-ring code on any platform. Each receiver is a thin GStreamer
front-end onto the front door that already existed — Windows ring texture,
Linux Wayland surface, Android Binder Surface.

## M21 — cross-platform Flutter WHIP sender ([sender/](sender/))

The counterpart to M20: a single Flutter app (Android, iOS, Windows, macOS,
Linux, web) that captures this device's **screen or camera** and casts it
*into* a vkrender scene over WHIP. Because the receivers run `whipserversrc`
(a WHIP *server*), the sender is just another WHIP client — **zero receiver
changes** beyond the discovery advertisement below.

- **Phase A (done, builds on web + Windows + Android)** — the reusable core:
  `whip_client.dart` (POST offer → `201` answer → `DELETE` on stop, no
  signaling server), `webrtc_sender.dart` (capture + `RTCPeerConnection` +
  **H.264 codec-preference forcing** — the Android receiver is H.264-only, so
  a default VP8 negotiation would connect but decode nothing there),
  `flutter_webrtc` 1.6.0. Android `mediaProjection` foreground service for
  `getDisplayMedia` on Android 14+. iOS/macOS permissions + entitlements +
  `NSBonjourServices` scaffolded (need a Mac to build; iOS screen share still
  wants a ReplayKit broadcast extension).
- **Phase B (done) — zero-config discovery.** All three receivers now
  advertise **`_vkrender-whip._tcp`** (TXT `path=/whip/endpoint`) on port
  8889, and the app browses for it via mDNS with a manual `host:port`
  fallback:
  - Windows — [client/whip_mdns_win.c](client/whip_mdns_win.c), a standalone
    `DnsServiceRegister` helper (deliberately *not* the AirPlay
    `dnssd_win.c`, which pulls in GPLv3 UxPlay headers — keeps `vkr_webrtc`
    MPL/LGPL). Built into `vkr_webrtc.exe`, verified compiling/linking.
  - Android — `NsdManager` registration in `WebrtcService.java`.
  - Linux — [scripts/webrtc_receiver_linux.sh](scripts/webrtc_receiver_linux.sh)
    runs the `whipserversrc → waylandsink` pipeline and `avahi-publish-service`
    together.
- **Pending** — iOS ReplayKit extension; live end-to-end phone-screen cast
  (same real-device bucket as the M16–M20 receivers).

## M22 — 12-presenter point-to-point room (Windows verified)

Up to **12 presenters** share their screens into one scene, point-to-point
(no SFU), over whichever path their device speaks: the Flutter sender or a
browser (WebRTC/WHIP), Miracast (Windows phones/laptops), or AirPlay (Apple).
The scene already composites 12 streams (M12) and mixed-protocol receivers
already coexist as separate producer processes; the gap was **WHIP fan-in**.

**Key discovery**: the gst-plugins-rs `whipserversrc` shipped with GStreamer
1.26 is **single-producer** — while one WHIP session is live, POSTs from
presenters 2..N are never answered (verified: HTTP timeouts, then `HTTP 000`
probes). No receiver-side branch code fixes that, so M22 owns the ingest
instead:

- **[client/whip_room.cpp](client/whip_room.cpp)** — a multi-session WHIP
  server built directly on `webrtcbin`: a tiny GLib-socket HTTP listener
  (portable to all three platforms) where each POSTed offer creates its OWN
  `webrtcbin` in the shared pipeline and is answered non-trickle (201 +
  SDP + `Location: /session/<id>`; DELETE tears down; OPTIONS handles CORS
  for browser senders; 503 when the room is full). Presenters who vanish
  without DELETE are reaped when ICE hits failed/closed. Same "own the
  front door" move as the M13 compositor and M14 pipe service.
- **Windows (verified)**: `vkr_webrtc` supplies the sink — each presenter's
  track is decoded and uploaded into its OWN `vkrender_client` connection
  (made lazily on the first frame so the ring matches the presenter's real
  resolution). The M14 service keys one stream per connection, so every
  presenter lands on their own cube face with zero renderer changes.
  **Measured: 12 concurrent `whipclientsink` presenters live at once, all
  streaming (600+ frames each), 13th cleanly rejected 503, distinct
  patterns on the cube faces in the snapshot, departed presenters reaped
  and their slots rejoinable.**
- **Linux (verified)**: same `whip_room` core, branches ending in
  `waylandsink` — each presenter's sink opens its own compositor connection,
  so each is its own scene stream. **Measured on melchior: 4 concurrent
  presenters, 600 adoptions/5s (4×30fps exactly), distinct patterns on the
  cube faces in the headless snapshot.**
- **Android (verified on the TCL tablet)**: same `whip_room` core (the
  single-producer whipserversrc AND the whole Rust plugin closure are gone —
  only the C webrtc/nice/dtls/srtp plugins remain, **which fixed the M20
  startup SIGSEGV**: the room now boots cleanly on device). The renderer's
  producer API grew **slots**: streams are keyed `(Binder UID, slot)` and
  the new `acquireInputSurfaceSlot(int)` AIDL methods (appended,
  binary-compatible) let the room APK hold up to 12 surfaces — the native
  room calls up to `WebrtcService` per presenter track for a slot Surface,
  and each presenter's H.264 AUs feed their own `AMediaCodec` (per-session
  M18 `AirplayVideoSink`). **Measured on device: 3 concurrent WHIP
  presenters (over `adb forward`, `whipclientsink video-caps=video/x-h264`)
  → slots 0/1/2 under one UID, ~434 adoptions/5s = 3×29 fps, all three
  patterns visible on the cubes.** Gotcha: BIND_RENDERER is
  signature-level — the renderer and room APKs must come from the same
  build (same debug keystore), else binding throws SecurityException (now
  caught + logged instead of crash-looping).

## M23 — receive-pipeline profiling: latency + drop causes (done, 2026-08-05)

Comprehensive instrumentation and measured analysis of every WHIP receive
pipeline — see **[docs/PROFILING.md](docs/PROFILING.md)** for the full
methodology, per-platform budget tables, drop taxonomy, and tuning knobs.
Highlights: one shared measurement model (`client/recv_stats.h`,
behind-live = pipeline clock − buffer PTS at ingress/sink probes, drops
counted by cause); **measured on all three platforms at target load with
zero unhealthy drops** (Windows: 12 presenters, 12×30 fps adopted exactly;
Linux: 4×30 exactly; Android device: 3×30 exactly at 60 Hz presents); key
finding — webrtcbin's 200 ms jitterbuffer `latency` is a reordering
*deadline*, not a constant delay (~3.4 ms ingress behind-live on a clean
link), tunable via `VKR_WEBRTC_LATENCY_MS`. AirPlay/RTSP paths can reuse
the same probes (documented, not yet wired); Miracast needs WinRT-side
stamps (documented).

## M24 — scene-out consumers + virtual camera (in progress)

The mirror image of the M14 producer path: a consumer process **negotiates
its own view of the scene** and receives it as a shared-texture ring —
GPU-side end to end. The on-screen windows become just one view among many.

- **Negotiation** (new pipe messages): `ConsumerHello{pid, w, h, fps,
  layout, streamMask}` → renderer answers `ConsumerConfig{accepted w/h/fps}`
  + a standard `PoolDesc` ring whose fence roles are the M14 contract with
  the renderer as producer (renderer signals ready, consumer signals
  consumed, `FrameReady` flows renderer→consumer). Layout ids and stream
  selection reuse the M15 vocabulary (two-cubes / single-cube / wall,
  per-stream curation).
- **Renderer**: per consumer, an off-screen render of that consumer's view
  at the negotiated resolution (own layout + stream subset), paced at the
  negotiated fps, blitted into the imported ring slot.
- **Consumer API** (client DLL + SDK release): `vkrConnectConsumer` /
  `vkrAcquireFrame` / `vkrReleaseFrame`; PPM test consumer for headless
  verification.
- **vkr_vcam.exe** — the scene as a webcam, two tiers sharing one consumer
  loop:
  - **DirectShow** (default; vendored MIT `softcam`): loads in the viewer
    app; Chrome/OBS/Zoom/classic Teams. Register once:
    `regsvr32 softcam.dll` (elevated).
  - **`--mf`** (Windows 11 `MFCreateVirtualCamera`; vendored MIT
    `VCamSample` re-identified as the vkrender camera): the media source
    runs in the OS Camera Frame Server, so the camera is visible to
    **everything** — MF/UWP (Windows Camera, new Teams) *and* DirectShow
    apps. Register once: `regsvr32 vkrender_vcam_source.dll` (elevated).
    Frame transport solves the user↔LocalService boundary by direction:
    the frame-server side (which holds `SeCreateGlobalPrivilege`) creates
    the `Global\` shared ring + mutex with a permissive DACL; the host
    opens by name and writes BGRA under the mutex. No feed → the source's
    built-in test pattern (a useful diagnostic). Later echoes:
    v4l2loopback (Linux).

## M25 — Windows presentation appliance (in progress)

Productize the Windows receiver stack:

- **Presentation layouts**: a `presentation` scene layout — flat tiles,
  count-adaptive: 1 session full screen, 2 top/bottom, 3 triangle, 4 quad
  (2×2). Fixed regions, aspect-fit letterboxing, auto-rearranged as
  sessions join/leave.
- **PIN authorization** (optional, per receiver) displayed as a Windows
  overlay when a sender initiates: Miracast natively
  (`MiracastReceiverAuthorizationMethod::PinDisplayRequired`); WHIP via
  `401` + `Authorization: Bearer <pin>` (sender apps prompt on 401);
  AirPlay via RAOP pin (implemented in the vkrender-airplay repo). Shared
  overlay helper: topmost layered window with large digits.
- **Virtual camera gating**: the service delivers consumer frames only
  while ≥1 producer is live — the camera idles until a presentation
  starts, then pumps automatically.
- **Installer** (Inno Setup): renderer + Miracast/WebRTC receivers +
  virtual camera DLLs (registered at install) + launcher; GStreamer
  runtime check; AirPlay receiver bundled from vkrender-airplay.

## Backlog — cloud calling via self-hosted LiveKit (M26 proposal)

Marry the system to a self-hosted SFU for internet reach, rooms, and auth.
Decisions sketched (2026-08-05): **LiveKit** (Apache-2.0, built-in TURN,
first-class Flutter SDK); receiver subscribes via `livekitwebrtcsrc` from
gst-plugins-rs — the same source-element-swap pattern as M20 (verified: our
melchior plugin build has `whepclientsrc`/`whepserversink` but needs a
rebuild with `--features livekit` for the LiveKit elements). Sender adopts
the `livekit_client` Flutter SDK (rooms/presence) in place of raw WHIP.
Plan: dev server on melchior → receiver subscribe → sender SDK → token
service + TURN/TLS hardening. Parked until the point-to-point room (M22) is
done.

## Later / ideas

- Audio: decode + share via existing IPC, or local playback in the decoder.
- Multiple streams → one renderer (N pipes, N pools, scene composition).
- Vulkan Video decode backend (`VK_KHR_video_decode_*` via FFmpeg Vulkan
  hwaccel) as a single-API cross-platform decoder, once driver support and
  the FFmpeg integration mature — would collapse the per-OS decoder backends.
- HDR: P010/HDR10 end-to-end (VideoProcessor → RGB10A2 on Windows, native
  P010 dma-buf on Linux) + `VK_EXT_hdr_metadata`.
