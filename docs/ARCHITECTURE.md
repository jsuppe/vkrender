# Architecture

vkrender is a two-process, zero-copy video pipeline. Process boundaries are a
feature: the network/decode half can crash, restart, or be sandboxed without
taking down the renderer, and the renderer never blocks on network I/O. The
cost that usually comes with process isolation — copying frames between
address spaces — is eliminated by sharing the GPU allocations themselves.

```
┌────────── vkdecoder ─────────────┐         ┌────────── vkrenderer ────────────┐
│ FFmpeg avformat (RTSP client)    │         │ GLFW window + Vulkan 1.3 (volk)  │
│ FFmpeg avcodec + D3D11VA (NVDEC) │  named  │ VkImage bound to imported        │
│ VideoProcessorBlt:               │  pipe   │ D3D11 texture memory             │
│   decode surface → shared BGRA   │ ──────► │ (VK_KHR_external_memory_win32)   │
│   texture ring (default 4)       │  msgs   │ Fullscreen sample + present      │
│ 2 shared D3D11 fences            │         │ Fences imported as timeline sems │
└──────────────────────────────────┘         └──────────────────────────────────┘
```

## 1. Process model and lifecycle

1. `vkdecoder` creates a named pipe (`\\.\pipe\<name>`, default `vkrender`)
   and blocks waiting for one client.
2. `vkrenderer` initializes Vulkan first, connects, and sends `Hello` with its
   PID and its Vulkan physical device LUID.
3. The decoder opens the correct DXGI adapter **by that LUID** (both processes
   must be on the same GPU or sharing silently breaks on hybrid systems),
   creates its D3D11 device, and opens the RTSP stream.
4. On the first decoded frame the decoder builds the shared pool and sends
   `PoolDesc`; from then on it's `FrameReady` per frame.
5. Either side exiting closes the pipe; the peer notices on its next
   send/poll. The renderer keeps displaying the last frame; the decoder exits.

## 2. Wire protocol

Defined in [common/protocol.h](../common/protocol.h). Transport is a named
pipe in message mode (one `WriteFile` = one message); every message is
`MsgHeader { magic, type, payloadSize }` + POD payload. The layer is
deliberately platform-neutral: handles travel as opaque `uint64` values whose
meaning is platform-defined, and `PoolDescMsg` carries plane-layout / DRM
modifier fields that Windows ignores but dma-buf platforms (Linux, Android)
need.

| Message | Direction | Purpose |
|---|---|---|
| `Hello` | renderer → decoder | renderer PID (for `DuplicateHandle`) + adapter LUID |
| `PoolDesc` | decoder → renderer | geometry, format, colorimetry, N texture handles, 2 fence handles |
| `FrameReady` | decoder → renderer | `{ slot, readyValue, ptsNs }` |
| `FrameConsumed` | renderer → decoder | reserved; the consumed fence is authoritative |
| `StreamReset` | decoder → renderer | pool is obsolete (resolution change); new `PoolDesc` follows |
| `Bye` | either | orderly shutdown |

## 3. Zero-copy memory sharing (Windows backend)

**Creation** (decoder): a ring of `DXGI_FORMAT_B8G8R8A8_UNORM` textures,
`MiscFlags = D3D11_RESOURCE_MISC_SHARED | SHARED_NTHANDLE`,
`BindFlags = RENDER_TARGET | SHADER_RESOURCE`. Each is exported once with
`IDXGIResource1::CreateSharedHandle` and the NT handle is duplicated into the
renderer with `DuplicateHandle` (the renderer's PID came from `Hello`).
Handle duplication happens **once per pool**, never per frame.

**Import** (renderer, [renderer/external_video_image.cpp](../renderer/external_video_image.cpp)):

1. Capability gate: `vkGetPhysicalDeviceImageFormatProperties2` with
   `VkPhysicalDeviceExternalImageFormatInfo{ D3D11_TEXTURE_BIT }` and the
   exact create parameters — fail loudly rather than discover problems at
   draw time.
2. `VkImage` with `VkExternalMemoryImageCreateInfo`, OPTIMAL tiling,
   `SAMPLED | TRANSFER_SRC`.
3. Memory type = intersection of `vkGetImageMemoryRequirements2` bits and
   `vkGetMemoryWin32HandlePropertiesKHR().memoryTypeBits` (skipping the
   intersection happens to work on NVIDIA and corrupts on other vendors).
4. `vkAllocateMemory` with `VkImportMemoryWin32HandleInfoKHR` +
   `VkMemoryDedicatedAllocateInfo` — imported D3D11 textures are
   dedicated-only on all desktop drivers.
5. Images are treated as permanently `VK_IMAGE_LAYOUT_GENERAL`. A transition
   out of `UNDEFINED` is legally allowed to discard contents, so the renderer
   never does one; each frame wraps sampling in
   `VK_QUEUE_FAMILY_EXTERNAL` acquire/release barriers instead.

**Why BGRA and not NV12?** Sharing the decoder's NV12 directly fails on
current NVIDIA Windows drivers: the imported image's plane-1 (chroma) offset
computed by the Vulkan driver does not match the D3D11 NV12 allocation
layout. Luma reads fine; chroma reads zeros and then GPU-faults
(`VK_ERROR_DEVICE_LOST` — verified empirically with plane-wise
copy-readbacks on RTX 5090 / driver 596.36). Win32 external memory has no
DRM-modifier equivalent to describe plane layouts explicitly, so this is not
fixable from the API side. The decoder therefore converts NV12 → BGRA with
the fixed-function D3D11 VideoProcessor (`VideoProcessorBlt`), which:

- replaces the `CopySubresourceRegion` that was needed anyway to get frames
  out of the decoder's recycled DPB array texture (still exactly **one** GPU
  operation per frame in the decoder, zero cross-process copies);
- performs BT.601/BT.709 + limited/full range conversion in fixed-function
  hardware (colorimetry from the stream is programmed into the VP);
- crops coded size (e.g. 1920×1088) to display size.

The renderer keeps a complete NV12 + `VkSamplerYcbcrConversion` import path
(`SharedFormat::NV12`) for dma-buf platforms where explicit plane layouts
make it correct.

## 4. Synchronization

Two shared `ID3D11Fence` objects (D3D11.4, `D3D11_FENCE_FLAG_SHARED`) are
exported like the textures and imported into Vulkan as **timeline
semaphores** (`VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT`, which the
spec defines to cover D3D11 fences). No keyed mutexes — timeline values give
N-deep pipelining with two monotonic counters and cannot deadlock on a
crashed peer.

- `frameReady` — decoder-signaled. After the blit into slot `i`:
  `Signal(readyFence, ++counter)` then `Flush()` (without the flush the
  signal can sit in the deferred command stream forever). `FrameReady`
  carries the value; the renderer's submit waits `readyReady ≥ value` on the
  GPU timeline — the CPU never waits.
- `frameConsumed` — renderer-signaled. When the renderer adopts a frame with
  value `V`, its submit signals `consumed = V − 1`, a promise that no frame
  with value `≤ V − 1` will ever be read again (skipped frames were never
  read; the previously displayed frame is done once this submit's wait
  chain retires). The decoder polls `GetCompletedValue()` (cheap CPU read)
  and may reuse slot `i` once `completed ≥ slotReadyValue[i]`.

Failure containment: if the renderer dies mid-frame, the decoder just stops
finding free slots, drops frames, and exits when the pipe closes — no fence
wait can hang it. If the decoder dies, the renderer's pending waits were
already satisfied (signal-then-send ordering) and it keeps presenting the
last frame.

## 5. Latency design

The pool is a **recycling ring, not a FIFO** — depth adds headroom, not
latency:

- The renderer drains all pending `FrameReady` messages every loop iteration
  and adopts only the newest (latest-frame-wins). Older undisplayed frames
  are skipped and their slots released.
- The decoder never queues: each frame goes straight from decode to blit to
  signal. When no slot is free it drops the frame, keeping the end-to-end
  path bounded instead of building a backlog.
- Swapchain uses `MAILBOX` present when available (newest-image-wins at the
  display, no tearing), falling back to FIFO.
- FFmpeg receive path runs with `AVFMT_FLAG_NOBUFFER`, `max_delay=100ms`,
  reduced probe window, and `AV_CODEC_FLAG_LOW_DELAY`. Hardware decode via
  D3D11VA avoids FFmpeg's frame-threading delay entirely.

Residual latency sources, in order: encoder GOP structure (B-frames add
irreducible reorder delay — configure the camera for no B-frames), network
jitter budget (`max_delay`), one vsync of display timing.

Default pool depth is 4 (`--pool-size`): one slot being written, one being
displayed (still referenced by in-flight GPU work for up to 2 frames), plus
slack for consumed-fence completion visibility. Below ~3 the decoder starts
dropping whenever the renderer hiccups; deeper than ~6 buys nothing.

## 6. Renderer frame loop

Per iteration ([renderer/main.cpp](../renderer/main.cpp)): poll GLFW → drain
IPC (handle `PoolDesc`/`StreamReset` with a device-idle rebuild; coalesce
`FrameReady`) → adopt newest frame → wait in-flight fence (2 frames in
flight) → acquire swapchain image → record: swapchain `UNDEFINED→COLOR`,
EXTERNAL acquire barrier, letterboxed fullscreen-triangle draw sampling the
imported image, EXTERNAL release barrier, `→PRESENT` → submit (waits:
swapchain acquire + `frameReady ≥ V`; signals: render-done + optionally
`consumed = V−1`) → present.

The video pass ([renderer/video_pass.cpp](../renderer/video_pass.cpp)) is
rebuilt per pool because the immutable sampler (which for NV12 pools carries
the `VkSamplerYcbcrConversion`) is baked into the pipeline layout.

## 7. Failure modes and debug switches

| Symptom | Meaning |
|---|---|
| `driver rejects D3D11 import config` at pool import | GPU/driver can't import this format — see capability gate in external_video_image.cpp |
| decoder `pool full, dropped N frames` | renderer stalled or consumed fence not advancing; benign if transient |
| renderer `vkQueueSubmit failed: -4` | device lost — use the env switches below to bisect |

Renderer env switches: `VKR_NO_VIDEO_DRAW` (import + sync, no sampling),
`VKR_NO_TIMELINE_WAIT`, `VKR_NO_EXTERNAL_BARRIER`, `VKR_DEBUG_COPY_TEST`
(one-shot copy-readback of the imported texture with a byte dump — this is
how the NV12 chroma-offset driver bug was isolated).

Validation layers load automatically in Debug builds when
`VK_LAYER_KHRONOS_validation` is installed (e.g. via the LunarG SDK).

## 8. Portability boundaries

Platform-specific code is confined to:

- `common/ipc_win32.cpp` — named pipe transport (`IpcChannel` API is neutral)
- `decoder/d3d11va_decoder.cpp` — decode + share + signal (implements the
  conceptual `IVideoDecoder` role; a `vaapi_decoder.cpp` /
  `mediacodec_decoder.cpp` slot in beside it)
- the Win32 import branch of `renderer/external_video_image.cpp`

The protocol, frame-loop, pass, and sync model are platform-neutral. See
[ROADMAP.md](../ROADMAP.md) for the Linux and Android plans.
