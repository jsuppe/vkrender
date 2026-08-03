# Android backend — renderer-owned Surface, BufferQueue zero-copy (M8)

Android is the third backend, and after two architecture generations it is
also the most platform-native: the renderer **owns an `AImageReader` and
hands its `Surface` to producers over Binder**. BufferQueue — the OS
machinery behind every Surface — then carries the frames cross-process with
its own queueing, fences, timestamps and backpressure. There is no custom
frame IPC at all.

## 1. Architecture

One APK, **two OS processes**:

```
┌── :decoder process ─────────────────┐    ┌── main process ──────────────────┐
│ DecoderService (foreground service) │    │ MainActivity : NativeActivity    │
│  binds RendererService ────────────────► │ RendererService (AIDL):          │
│  ◄─── Surface (Parcelable) ─────────│    │   returns the reader's Surface   │
│  └ decoder_jni → SurfaceDecoder     │    │  └ android_main.cpp              │
│ FFmpeg avformat (RTSP demux only)   │ Buf │ owns AImageReader (GPU_SAMPLED) │
│ AMediaCodec decodes INTO the        │ferQ │ per vsync: acquireLatestImage → │
│  renderer's Surface;                │ueue │  AHardwareBuffer → Vulkan import │
│  releaseOutputBuffer = delivery     │───► │  (cached per buffer) → sample    │
└─────────────────────────────────────┘    └──────────────────────────────────┘
```

- **AIDL** ([IVkRendererService.aidl](../android/app/src/main/aidl/dev/vkrender/IVkRendererService.aidl)):
  `Surface acquireInputSurface()`. `Surface` is Parcelable, so the Binder
  handoff is trivial; the native bridge converts the reader's window with
  `ANativeWindow_toSurface`. Because *any* Surface producer works —
  MediaCodec, camera, another app if the service were exported with
  permission gating — the renderer is effectively a video compositor service.
- **Decode** ([decoder/surface_decoder.cpp](../decoder/surface_decoder.cpp)):
  FFmpeg avformat demux (minimal static build, §3) feeds `AMediaCodec`
  configured directly with the renderer's Surface.
  `releaseOutputBuffer(render=true)` *is* frame delivery — timestamped into
  the BufferQueue. Backpressure is automatic: if the consumer holds all
  buffers, `dequeueOutputBuffer` starves and decode stalls. RTSP reconnect
  with backoff and the wall-clock watchdog are internal; the session ends
  only when the surface dies (renderer gone), and the Java side re-binds and
  starts a new session.
- **Render** ([renderer/android_main.cpp](../renderer/android_main.cpp)): one
  `AImageReader_acquireLatestImage` per Choreographer vsync (stale frames are
  discarded by the call itself — latest-wins for free, fences honored by the
  sync acquire). Each distinct `AHardwareBuffer` (they recycle) is imported
  into Vulkan once via the external-format + YCbCr-conversion path and
  cached; the acquired `AImage` is held while its buffer is being sampled and
  deleted only after the frame fence that last read it retires — the entire
  consumed-handshake of previous generations reduced to a local vector.

What M8 deleted relative to M7 (see git history for the old design): the
Unix-socket frame protocol, AHB handle passing, the SurfaceDesc/FrameReady/
FrameConsumed messages, the decoder-side held-ring and drop policy, the
acquire-fence fd plumbing and the renderer's pending-window PTS logic —
BufferQueue provides all of it.

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

## 3. GStreamer for Android (M9 transport/demux)

Transport, demux and parsing run on GStreamer
(`rtspsrc → parsebin → appsink` emitting byte-stream access units; parsebin's
dynamically-plugged h264/h265parse gets `config-interval=-1` so SPS/PPS stay
in-band for MediaCodec). The decode element remains platform `AMediaCodec`
into the renderer's Surface — GStreamer's `amcvideodec` wraps the same codec
but cannot target a foreign AImageReader surface without an extra GL copy.

Dependencies come from the **official GStreamer Android universal prebuilts**:
download `gstreamer-1.0-android-universal-<ver>.tar.xz` from
gstreamer.freedesktop.org and extract the `arm64-v8a` and `x86_64` trees to
`android/gstreamer-android/<abi>/` (gitignored). The CMake build resolves the
static libraries and their transitive dependencies via the prebuilts'
pkg-config metadata (host `pkg-config` required — the GStreamer Windows devel
install or any Linux distro provides one) and links the needed plugins
statically (`coreelements app rtsp rtp rtpmanager udp tcp playback
typefindfunctions videoparsersbad`), registered via
`GST_PLUGIN_STATIC_REGISTER` at init. GStreamer warnings/errors route to
logcat under the `vkrender-gst` tag.

(The M7-era minimal FFmpeg build and
[scripts/build_ffmpeg_android.sh](../scripts/build_ffmpeg_android.sh) remain
in history/tree but are no longer part of the build.)

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
- **Hardware acquire fences signal 1-2 frames after the output callback**,
  which broke naive adoption twice: blocking on the newest fence stalled
  presentation (~10 drops/s), and non-blocking latest-only adoption
  livelocked (the newest frame's fence is always the youngest, so
  almost-ready frames kept being replaced — heavy visible frame skipping
  with near-zero decoder drops). The renderer now keeps a small pending
  window and adopts the **newest frame that is both fence-ready and
  PTS-due** (clock-anchored pacing, re-anchoring on discontinuities), with
  an 8-deep decoder ring absorbing the fence latency. Measured on the TCL
  tablet: 150 adoptions / 5 s — exactly the stream's 30 fps — with zero
  drops. A 5-second adoption/present rate log makes this verifiable on any
  device.
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
