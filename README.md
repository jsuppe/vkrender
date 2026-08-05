# vkrender — two-process zero-copy RTSP → Vulkan video pipeline

Two cooperating processes that share decoded video frames through GPU memory,
with **zero copies between processes**: the texture the decoder writes is the
exact same GPU allocation the renderer samples. Runs on **Windows**
(D3D11VA → NT-handle sharing), **Linux** (VAAPI → dma-buf sharing — the
renderer samples the decoder's decode surfaces directly, no conversion pass
at all), and **Android** (MediaCodec → AHardwareBuffer sharing between an
activity and a service in separate processes). Platform deep-dives:
[docs/LINUX.md](docs/LINUX.md) · [docs/ANDROID.md](docs/ANDROID.md).

All decode pipelines are built on **GStreamer** (M9); the decode element
itself stays platform-native (D3D11 decoders, VA-API, MediaCodec).

```
┌────────── vkdecoder ─────────────┐         ┌────────── vkrenderer ────────────┐
│ GStreamer rtspsrc → decodebin3   │         │ GLFW + Vulkan 1.3 (volk)         │
│  → d3d11 decoders (our device)   │  named  │ VkImage imported from NT handle  │
│ VideoProcessorBlt:               │  pipe   │ (VK_KHR_external_memory_win32,   │
│   decode surface → shared BGRA   │ ──────► │  D3D11_TEXTURE, dedicated alloc) │
│   texture ring (default 4 slots) │         │ Timeline-semaphore sync          │
│ Shared D3D11 fences (x2)         │         │ (imported D3D11 fences)          │
└──────────────────────────────────┘         └──────────────────────────────────┘
```

Deep dive: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) · Plans: [ROADMAP.md](ROADMAP.md)

- **Textures**: ring of `B8G8R8A8_UNORM` textures created with
  `D3D11_RESOURCE_MISC_SHARED | SHARED_NTHANDLE`, exported via
  `IDXGIResource1::CreateSharedHandle`, duplicated once into the renderer
  process, imported with `VkImportMemoryWin32HandleInfoKHR` +
  `VkMemoryDedicatedAllocateInfo`.
- **Sync**: two shared `ID3D11Fence` objects imported into Vulkan as timeline
  semaphores (`VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT`).
  `frameReady` is signaled by the decoder after each blit; `frameConsumed` is
  signaled by the renderer to release older slots. Monotonic 64-bit values,
  no keyed mutex, no CPU waits in the render loop.
- **Protocol** ([common/protocol.h](common/protocol.h)): `Hello` (renderer pid +
  Vulkan device LUID, which drives the decoder's adapter choice), `PoolDesc`
  (handles + geometry + colorimetry), `FrameReady { slot, fenceValue, pts }`,
  `StreamReset`. Transport is a named pipe; the message layer is
  platform-agnostic and carries dma-buf plane layouts for a future Linux
  (VAAPI) backend.

## Why BGRA and not NV12?

Sharing the NV12 texture directly and sampling it in Vulkan (via
`VkSamplerYcbcrConversion` or plane views) **does not work on current NVIDIA
Windows drivers** (tested: RTX 5090, driver 596.36): the imported image's
luma plane reads correctly, but the driver's computed plane-1 (chroma) offset
does not match the D3D11 NV12 allocation layout — chroma reads return garbage
and GPU-fault the device (`VK_ERROR_DEVICE_LOST`). Win32 external memory has
no equivalent of DRM format modifiers to describe plane layouts explicitly.

So the decoder does the NV12→BGRA conversion with the fixed-function D3D11
VideoProcessor (`VideoProcessorBlt`, which also handles BT.601/709 and range,
and crops coded size → display size). This replaces the GPU copy that would
otherwise be needed to get frames out of the decoder's DPB array texture, so
the pipeline still performs exactly **one** GPU operation per frame in the
decoder and **zero** copies between processes. On Linux, VAAPI dma-buf export
carries explicit plane layouts, so the NV12 path (already implemented in the
renderer) should work there.

## Build

No Vulkan SDK required on either OS — Vulkan-Headers, volk, GLFW and glslang
are fetched by CMake.

**Windows** (VS 2022 C++ workload, CMake 3.24+, and the GStreamer MSVC
x86_64 **runtime + devel** MSIs from gstreamer.freedesktop.org):

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

**Linux** (GStreamer ≥ 1.22 + libva from the distro; full details, NVIDIA
setup, and no-sudo bootstrap in [docs/LINUX.md](docs/LINUX.md)):

```bash
sudo apt install cmake g++ pkg-config libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev \
  libva-dev libdrm-dev xorg-dev libwayland-dev libxkbcommon-dev wayland-protocols
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel && ctest --test-dir build
```

## Run

Start the decoder (it waits for the renderer on the pipe), then the renderer:

```bash
build/Release/vkdecoder.exe --url rtsp://camera:8554/stream
```

```bash
build/Release/vkrenderer.exe
```

Options: `--pipe <name>` on both ends to run multiple instances;
`--pool-size <2-8>` on the decoder (default 4);
`--pacing latest|pts` on the renderer (default `latest` = lowest latency,
`pts` = smooth clock-anchored playback).

**Linux compositor mode (M13)**: `vkrenderer --wayland [socket]` turns the
renderer into a minimal Wayland compositor — any Wayland client (GStreamer
`waylandsink`, mpv, ...) becomes a zero-copy video producer, one stream per
client on the two-cube scene. See [ROADMAP.md](ROADMAP.md#m13) and
docs/LINUX.md.

**Windows service mode (M14)**: `vkrenderer --serve [pipe]` +
[client/vkrender_client.h](client/vkrender_client.h) — a four-call C API
(`vkrConnect` / `vkrDequeueBuffer` / `vkrQueueBuffer` / `vkrDisconnect`)
gives any D3D11 app a renderer-owned shared-texture ring with GPU-side
fencing; each client is one stream on the scene. Demo:
`vkr_client_demo.exe`.

**AirPlay receiver on Android (M18)**: a third APK
(`dev.vkrender.airplay`, install alongside the renderer app) makes the
tablet an AirPlay mirroring sink — iPhones/iPads/Macs see "vkrender" in
Screen Mirroring and the mirror lands on the cube. GPLv3 (vendored UxPlay
core); needs a multicast-friendly Wi-Fi network.

**AirPlay receiver (M17, Linux)**: run the compositor
(`vkrenderer --wayland vkrender-0 --headless -1` on a headless box) and
`WAYLAND_DISPLAY=vkrender-0 uxplay -n vkrender -nh -vs waylandsink` —
Apple devices see "vkrender" in Screen Mirroring and the mirror becomes a
scene stream. (`apt install uxplay`; avahi must be running.)

**Miracast receiver (M16, Windows)**: `vkr_miracast.exe` makes the machine
a discoverable cast target ("vkrender") and feeds the cast into the scene
as a normal producer stream. Disable Windows' "Projecting to this PC"
first; phones need a Miracast-capable Wi-Fi adapter on the receiver,
Windows senders work over the LAN.

**Multi-window + controls (M15, `--serve`/`--wayland`)**: repeatable
`--window WxH+X+Y` spans one seamless scene across windows/displays.
Runtime keys: `L` layout (two-cubes / single-cube / stream wall), `SPACE`
pause, `-`/`=` speed, `B` backdrop, `1`-`9` remove/restore a stream from
the scene (`0` all), `P` snapshot each window to `scene-w<N>.ppm`, `ESC`
quit.

Codecs: H.264 and H.265 (8-bit and 10-bit) verified via the D3D11 hardware
decoders; 10-bit sources are shared as RGB10A2. AV1 should work on
AV1-capable GPUs but is untested (no AV1-over-RTSP local loop test).

Both processes are resilient: the decoder auto-reconnects to the RTSP source
with backoff and serves renderer restarts; the renderer redials a restarted
decoder and shows the last frame (with status in the title bar) during any
outage.

### Latency

The design is latest-frame-wins end to end: the pool is a recycling ring
(depth adds drop-resistance headroom, **not** display latency), the renderer
always adopts the newest decoded frame and skips stale ones, presentation
uses MAILBOX when the driver offers it, and the RTSP receive path runs
with a 100 ms jitter cap (`rtspsrc latency=100`). Residual latency comes from the encoder
(disable B-frames at the source) and one vsync of display timing. Details in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#5-latency-design).

### Local test stream

```bash
# terminal 1: RTSP server (https://github.com/bluenviron/mediamtx)
mediamtx.exe

# terminal 2: publish a test pattern (any ffmpeg CLI build)
ffmpeg -re -f lavfi -i testsrc2=size=1920x1080:rate=30 -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p -f rtsp rtsp://127.0.0.1:8554/test
```

then `vkdecoder --url rtsp://127.0.0.1:8554/test` + `vkrenderer`.

## Debug switches (renderer, env vars)

- `VKR_NO_VIDEO_DRAW=1` — import and sync but never sample (isolates sampling issues)
- `VKR_NO_TIMELINE_WAIT=1` — skip the frameReady wait
- `VKR_NO_EXTERNAL_BARRIER=1` — skip EXTERNAL queue-family barriers
- `VKR_DEBUG_COPY_TEST=1` — one-shot copy-readback of the imported texture with byte dump

Validation layers are enabled automatically in Debug builds when
`VK_LAYER_KHRONOS_validation` is present on the system (e.g. LunarG SDK installed).

## Layout

```
common/    protocol + IPC (named pipe on Windows, SEQPACKET UDS on POSIX)
decoder/   GStreamer RTSP + platform decode, shared pool, fences (vkdecoder)
renderer/  Vulkan context, external-memory import, video pass   (vkrenderer)
android/   two APKs: renderer service app (Vulkan cube) + decoder client app (see docs/ANDROID.md)
scripts/   Android GStreamer prebuilt fetch/prune, legacy FFmpeg cross-build
```

## Roadmap

See [ROADMAP.md](ROADMAP.md) — all milestones through M9 (GStreamer decode
pipelines on Windows, Linux, and Android) are complete; the backlog and
future ideas live there and in the GitHub issues.
