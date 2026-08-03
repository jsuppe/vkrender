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

## Later / ideas

- Audio: decode + share via existing IPC, or local playback in the decoder.
- Multiple streams → one renderer (N pipes, N pools, scene composition).
- Vulkan Video decode backend (`VK_KHR_video_decode_*` via FFmpeg Vulkan
  hwaccel) as a single-API cross-platform decoder, once driver support and
  the FFmpeg integration mature — would collapse the per-OS decoder backends.
- HDR: P010/HDR10 end-to-end (VideoProcessor → RGB10A2 on Windows, native
  P010 dma-buf on Linux) + `VK_EXT_hdr_metadata`.
