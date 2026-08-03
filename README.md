# vkrender — two-process zero-copy RTSP → Vulkan video pipeline

Two cooperating processes that share decoded video frames through GPU memory,
with **zero copies between processes**: the texture the decoder writes is the
exact same GPU allocation the renderer samples.

```
┌────────── vkdecoder ─────────────┐         ┌────────── vkrenderer ────────────┐
│ FFmpeg avformat (RTSP)           │         │ GLFW + Vulkan 1.3 (volk)         │
│ FFmpeg avcodec + D3D11VA (NVDEC) │  named  │ VkImage imported from NT handle  │
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

Prereqs: Visual Studio 2022 (C++ workload), CMake 3.24+, a Vulkan-capable GPU
driver. No Vulkan SDK required — Vulkan-Headers, volk, GLFW and glslang are
fetched by CMake; prebuilt FFmpeg 7.1 (BtbN shared) is downloaded at
configure time.

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
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
`--pool-size <2-8>` on the decoder (default 4).

### Latency

The design is latest-frame-wins end to end: the pool is a recycling ring
(depth adds drop-resistance headroom, **not** display latency), the renderer
always adopts the newest decoded frame and skips stale ones, presentation
uses MAILBOX when the driver offers it, and the FFmpeg receive path runs
unbuffered with a 100 ms jitter cap. Residual latency comes from the encoder
(disable B-frames at the source) and one vsync of display timing. Details in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#5-latency-design).

### Local test stream

```bash
# terminal 1: RTSP server (https://github.com/bluenviron/mediamtx)
mediamtx.exe

# terminal 2: publish a test pattern (ffmpeg.exe from build/ffmpeg-prebuilt/*/bin)
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
common/    protocol + named-pipe IPC (platform-agnostic API, win32 backend)
decoder/   FFmpeg RTSP + D3D11VA decode, shared pool, fences  (vkdecoder.exe)
renderer/  Vulkan context, external-memory import, video pass (vkrenderer.exe)
cmake/     prebuilt FFmpeg download/import targets
```

## Roadmap

See [ROADMAP.md](ROADMAP.md) — Windows hardening (M5), Linux VAAPI/dma-buf
backend (M6), Android MediaCodec/AHardwareBuffer backend (M7).
