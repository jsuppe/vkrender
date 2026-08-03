# Android backend — MediaCodec + AHardwareBuffer zero-copy

Android is the third backend, and its sharing primitive is the most
first-class of all: `AHardwareBuffer` is Android's kernel-backed,
refcounted, cross-process GPU buffer, with a dedicated API for passing it
over Unix sockets and a dedicated Vulkan extension for importing it.

## 1. Architecture

One APK, **two OS processes** — the Android-idiomatic mapping of the desktop
process pair:

```
┌── :decoder process ─────────────────┐    ┌── main process ──────────────────┐
│ DecoderService (Java shim)          │    │ MainActivity : NativeActivity    │
│  └ decoder_jni → MediaCodecDecoder  │    │  └ android_main.cpp              │
│ FFmpeg avformat (RTSP demux only,   │ AF │ Vulkan renderer (same video_pass │
│  static minimal build)              │UNIX│  / pool code as desktop)         │
│ AMediaCodec hw/sw decode            │sock│ VkImage ← AHardwareBuffer import │
│  └ output → AImageReader surface    │──► │ (VK_ANDROID_external_memory_ahb, │
│ AImage → AHardwareBuffer per frame  │    │  VkExternalFormatANDROID +       │
│  sent via sendHandleToUnixSocket    │    │  YCbCr conversion)               │
└─────────────────────────────────────┘    └──────────────────────────────────┘
```

- **Manifest** ([android/app/src/main/AndroidManifest.xml](../android/app/src/main/AndroidManifest.xml)):
  `DecoderService` has `android:process=":decoder"` — a real process boundary,
  same UID. `MainActivity extends NativeActivity` starts the service with the
  RTSP url from its own intent, then hands control to native code.
- **IPC**: the same `IpcChannel` protocol as desktop over an
  **abstract-namespace** Unix socket (no filesystem node — works in the app
  sandbox). `SurfaceDesc` with `numObjects == 0` announces "an
  `AHardwareBuffer` follows out-of-band", then
  `AHardwareBuffer_sendHandleToUnixSocket` /
  `AHardwareBuffer_recvHandleFromUnixSocket` transfer the buffer on the same
  socket. `FrameReady` / `FrameConsumed` / `StreamStatus` / reconnect
  semantics are identical to desktop.
- **Decode** ([decoder/mediacodec_decoder.cpp](../decoder/mediacodec_decoder.cpp)):
  FFmpeg's avformat does RTSP demux exactly as on desktop (a ~3 MB static
  build with **no decoders** — see §3); Annex-B access units feed NDK
  `AMediaCodec`, whose output surface is an `AImageReader` created with
  `AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE`. Every decoded frame surfaces as
  an `AImage` wrapping a gralloc `AHardwareBuffer` — the exact allocation the
  renderer samples. Zero copies, zero conversions.
- **Buffer identity & lifetime**: the reader cycles a fixed buffer set; the
  `AHardwareBuffer*` value identifies a buffer, announced once. Held
  `AImage`s pin buffers until `FrameConsumed` releases them
  (`AImage_delete` returns the buffer to the codec) — the same held-ring
  policy as the VAAPI backend, capped so a stalled renderer drops frames
  instead of queueing latency.
- **Import** ([renderer/external_video_image.cpp](../renderer/external_video_image.cpp)
  `addSurfaceAHB`): `vkGetAndroidHardwareBufferPropertiesANDROID` →
  `VkExternalFormatANDROID` (vendor-opaque YUV) → `VkSamplerYcbcrConversion`
  built from the buffer's suggested model/range/siting — the mandatory
  external-format path — then dedicated import + bind + sampled view. The
  renderer pipeline (video_pass, barriers with `VK_QUEUE_FAMILY_FOREIGN_EXT`,
  latest-frame-wins loop) is shared desktop code.

## 2. Building

Prereqs: Android SDK (API 34+ platform, build-tools), NDK r27, CMake 3.22 in
the SDK, JDK 17+, and the prebuilt FFmpeg static libs (§3).

```bash
# android/local.properties must point at your SDK: sdk.dir=...
gradle -p android assembleDebug     # or open android/ in Android Studio
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
```

The Gradle module drives the repo's sources through
[android/app/src/main/cpp/CMakeLists.txt](../android/app/src/main/cpp/CMakeLists.txt)
(volk + Vulkan-Headers via FetchContent; `x86_64` and `arm64-v8a` ABIs).
Shader SPIR-V ships as APK assets (`android/app/src/main/assets/shaders/`,
regenerate from a desktop build's `build/shaders/`).

## 3. FFmpeg for Android

RTSP demux uses a deliberately tiny FFmpeg: avformat/avcodec/avutil with
`--disable-everything` plus only the RTSP/RTP/SDP demuxers, TCP/UDP/RTP
protocols, H.264/HEVC parsers and Annex-B bitstream filters. No decoders, no
encoders, no scaling — decode belongs to MediaCodec. Build it on any Linux
box with [scripts/build_ffmpeg_android.sh](../scripts/build_ffmpeg_android.sh)
(downloads the Linux NDK, builds both ABIs, ~3 MB total) and place the result
at `android/ffmpeg-android/<abi>/` (gitignored).

## 4. Running against a local test stream

With the emulator, the host's loopback is `10.0.2.2`. **Pitfall discovered
the hard way: the emulator's own gRPC service listens on host port 8554 — the
RTSP default — so run the RTSP server on another port** (the guest's connect
to 10.0.2.2:8554 reaches the emulator's HTTP/2 listener and RTSP fails with
"Invalid data").

```bash
# host: RTSP server on 8556 + test pattern publisher
MTX_RTSPADDRESS=:8556 ./mediamtx &
ffmpeg -re -stream_loop -1 -f lavfi -i testsrc2=size=1280x720:rate=30 \
  -c:v libx264 -preset veryfast -tune zerolatency -g 30 -pix_fmt yuv420p \
  -f rtsp rtsp://127.0.0.1:8556/test &

# app
adb shell am start -n dev.vkrender/.MainActivity --es url rtsp://10.0.2.2:8556/test
adb logcat -s vkrender:*        # pipeline logs from both processes
```

Expected logs: `decoder: stream: video/avc WxH` → `stream header sent` →
`renderer: imported AHardwareBuffer surface key=... (N total, zero-copy)`
(one per distinct buffer, then they cycle) → continuous
`codec output frame N` counters.

## 5. Emulator vs real devices

| Aspect | Emulator (verified 2026-08-03) | Real device: TCL 8483A tablet, Android 15, arm64 (**verified 2026-08-03**) |
|---|---|---|
| Codec | `c2.goldfish.h264.decoder` **stalls in surface mode** — the decoder auto-detects `ro.boot.qemu` and picks `c2.android.avc.decoder` (software) instead | vendor hardware codec via `createDecoderByType` ✅ |
| AHB→Vulkan | works via gfxstream (host GPU) | ✅ vendor-private external format (0xef) imported through `VkExternalFormatANDROID`; hardware pool of 19 buffers |
| Colors | **wrong (chroma routing)**: gfxstream's external-format YV12 sampling mis-routes chroma planes; swizzle overrides ignored. Cosmetic; video animates, geometry/luma correct | ✅ **correct** — vendor YCbCr conversion, full test-pattern palette |
| Performance | 30 fps, warm-up drop only | ✅ 30 fps, 1 warm-up drop over 45 s (after the fence-latency tuning below) |

Real-device notes (found and fixed during verification):
- **Swapchain preTransform**: the panel is mounted rotated; accepting
  `currentTransform` without rotating in the shader showed the image 180°
  flipped. The swapchain now prefers `IDENTITY` (compositor applies the
  display rotation).
- **Hardware acquire fences signal 1-2 frames after the output callback.**
  Blocking on them in the render loop (or sizing the held ring too small)
  backs up the consumed handshake — observed as ~10 drops/s. Adoption is now
  a non-blocking fence poll (present the current frame until the new one's
  fence signals) and the held ring is 8 deep, which absorbs the fence latency
  without adding display latency (latest-wins still skips stale frames).
- A convenient transport for desk testing: `adb reverse tcp:8556 tcp:8556`
  lets the device reach the host's RTSP server at `rtsp://127.0.0.1:8556/...`
  over the USB cable — no LAN configuration.

## 6. Sync model

Producer side: `AImageReader_acquireNextImageAsync` hands over each image
immediately with an **acquire fence fd**; the fence rides to the renderer as
SCM_RIGHTS data on the `FrameReady` message. The renderer waits on it just
before sampling — sync_file fds are pollable, so this needs no Vulkan
extension (a `SYNC_FD` semaphore import is a drop-in refinement if ever
needed) and the decoder never blocks on frame readiness. Skipped frames'
fences are closed unread. Consumer side: `FrameConsumed` after the renderer's
fences prove its reads retired — same design as Linux. Remaining refinement:
release fences via `AImage_deleteAsync` for the consumed direction.

## 7. Lifecycle (hardened in M7.1)

- The decoder runs as a **foreground service** (`dataSync` type, persistent
  notification): the cached-app freezer no longer suspends it on screen-off.
  Verified: decode continued at full rate through a 90 s screen-off cycle.
- A wall-clock watchdog (`AVIOInterruptCB`) aborts any demux call exceeding
  its deadline — after a freeze/thaw of the *renderer-visible* kind or a
  server-side session death, the decoder falls into its normal
  reconnect-with-backoff path instead of hanging on a stale socket.
- While the activity is paused the renderer keeps pumping IPC and
  consume-discards frames (closing their fences), so the decoder's buffer
  ring never starves; on wake the activity's replacement `ANativeWindow` is
  detected and the Vulkan surface + swapchain are rebuilt (verified across
  sleep/wake).

## 8. Known limitations / backlog

- Real-device verification (colors, hardware codecs, arm64) —
  [#5](https://github.com/jsuppe/vkrender/issues/5).
- Emulator color mis-routing (gfxstream YV12) — cosmetic, emulator-only (§5).
- Release-fence direction of §6.
- Rotation is handled as a window replacement (surface rebuild); no
  orientation-specific handling beyond that.
