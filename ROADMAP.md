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

## M5 — Windows hardening

- [ ] Renderer reconnect loop: survive decoder restart, re-listen/re-dial the
      pipe, reimport on the next `PoolDesc` (today: shows last frame, no rejoin).
- [ ] Decoder RTSP auto-reconnect with backoff; surface stream state to the
      renderer (title/OSD).
- [ ] PTS-based presentation pacing option (smooth playback mode) alongside
      latest-frame-wins (lowest latency mode); A/V clock plumbing for future audio.
- [ ] H.265/AV1 streams (D3D11VA already supports both — needs testing), and
      10-bit content (P010 → BGRA10/RGB10A2 via VideoProcessor).
- [ ] CI: GitHub Actions Windows build.

## M6 — Linux backend (VAAPI + dma-buf)

Goal: same two binaries on Linux with true NV12 zero-copy (no conversion pass
needed — dma-buf carries explicit plane layouts, fixing what Windows can't
express).

Plan:
1. **IPC**: `common/ipc_posix.cpp` — Unix domain socket implementing the
   existing `IpcChannel` API; fds are passed in-band with `SCM_RIGHTS`
   ancillary data instead of raw handle values (`SharedFrameHandle` already
   models this).
2. **Decoder**: `decoder/vaapi_decoder.cpp` — FFmpeg `AV_HWDEVICE_TYPE_VAAPI`
   decode; export each frame's surface with
   `vaExportSurfaceHandle(VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
   VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS)` →
   dma-buf fd(s) + per-plane offset/pitch + DRM format modifier, all of which
   go into `PoolDescMsg`. Decode-surface recycling differs from D3D11 (VAAPI
   surfaces are refcounted via `AVFrame`), so the pool becomes a ring of
   held `AVFrame` refs rather than copies — potentially removing even the
   in-process blit.
3. **Renderer import**: extend `external_video_image.cpp` with the dma-buf
   branch: `VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`
   (`VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT`,
   `VkImageDrmFormatModifierExplicitCreateInfoEXT` with the exported plane
   layouts). NV12 sampling path (`VkSamplerYcbcrConversion`) already exists.
4. **Sync**: start with dma-buf implicit sync (correct on today's Mesa/NVIDIA
   with `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` fallback); explicit path via
   `VK_KHR_external_semaphore_fd` (SYNC_FD) mirroring the timeline design.
5. **Build**: drop the Windows-only guard in CMake; FFmpeg from system
   packages (pkg-config) instead of BtbN prebuilt; GLFW/Wayland+X11.

Risks: modifier negotiation between the VAAPI driver and the Vulkan driver on
hybrid setups (iGPU decode + dGPU render needs a linear-modifier or
cross-device path); NVIDIA proprietary VAAPI is weak — NVDEC path may be
wanted later (`AV_HWDEVICE_TYPE_CUDA` → `VK_KHR_external_memory_fd` opaque).

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
