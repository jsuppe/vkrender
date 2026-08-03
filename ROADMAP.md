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
- [ ] Deferred to M6.1: explicit GPU-side sync via exported sync_files
      (`VK_KHR_external_semaphore_fd`); Intel/AMD combined-mode validation on
      real hardware; 10-bit (P010) Linux path; hybrid-GPU (decode≠render
      device) support.

## M7 — Android backend (MediaCodec + AHardwareBuffer)

Goal: decoder service and renderer app as separate processes sharing frames
through `AHardwareBuffer` — Android's first-class cross-process GPU buffer.

Plan:
1. **Decode**: NDK `AMediaCodec` in surface-less mode with an
   `AImageReader` configured with
   `AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE`; each output `AImage` exposes an
   `AHardwareBuffer` (typically YUV420/NV12-class, `AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420`).
   RTSP demux still via FFmpeg (avformat builds cleanly for Android) feeding
   codec-specific NAL units to MediaCodec.
2. **Share**: `AHardwareBuffer_sendHandleToUnixSocket` /
   `recvHandleFromUnixSocket` over the same Unix-socket IPC as Linux — the
   protocol's opaque-handle model fits directly; pool slots are AImages held
   until the consumer releases them.
3. **Import**: `VK_ANDROID_external_memory_android_hardware_buffer` with
   `VkExternalFormatANDROID` (the mandatory path for YUV buffers) +
   `VkSamplerYcbcrConversion` created from
   `vkGetAndroidHardwareBufferPropertiesANDROID` format properties. This is
   the renderer's third import branch, structurally identical to dma-buf.
4. **Sync**: MediaCodec/AImageReader deliver release fences as sync fds →
   import with `VK_KHR_external_semaphore_fd` (SYNC_FD, temporary import) —
   maps onto the existing per-frame wait design; consumed direction returns a
   fence fd with the buffer release.
5. **Process model**: decoder as a bound service (or simply a second
   executable under the same UID for the demo); renderer as a GLFW-free
   NativeActivity (android_native_app_glue + `VK_KHR_android_surface`) —
   windowing is the only renderer piece GLFW doesn't cover on Android.
6. **Build**: NDK toolchain file, Gradle wrapper for packaging; shaders
   precompiled at desktop build time (glslang already in-tree).

Risks: vendor codecs may emit private tiled formats — always use
`ExternalFormatANDROID` + YCbCr conversion rather than assuming NV12;
`sendHandleToUnixSocket` requires both ends in the same user namespace
(fine for same-app services, revisit for true multi-app).

## Later / ideas

- Audio: decode + share via existing IPC, or local playback in the decoder.
- Multiple streams → one renderer (N pipes, N pools, scene composition).
- Vulkan Video decode backend (`VK_KHR_video_decode_*` via FFmpeg Vulkan
  hwaccel) as a single-API cross-platform decoder, once driver support and
  the FFmpeg integration mature — would collapse the per-OS decoder backends.
- HDR: P010/HDR10 end-to-end (VideoProcessor → RGB10A2 on Windows, native
  P010 dma-buf on Linux) + `VK_EXT_hdr_metadata`.
