# Linux backend — VAAPI + dma-buf zero-copy

This document covers the Linux implementation in exhaustive detail: how the
zero-copy path works, exactly what to install on which hardware, how to build,
how to verify the pipeline end-to-end (including on a headless box over SSH),
and how to debug it when a driver misbehaves.

## 1. How Linux zero-copy differs from Windows

On Windows the decoder *converts* each frame (NV12 → BGRA via the D3D11
VideoProcessor) into a ring of shareable textures, because D3D11→Vulkan NV12
import is broken on current NVIDIA drivers and Win32 external memory cannot
describe plane layouts explicitly.

Linux needs **neither the conversion nor the ring copy**. Three platform
facilities make the decoder's *own decode surfaces* directly shareable:

1. **`vaExportSurfaceHandle`** (libva ≥ 1.4, `DRM_PRIME_2`) exports any VAAPI
   surface as a dma-buf fd **plus an explicit description**: DRM fourcc, DRM
   format modifier (the tiling layout), and per-plane offsets/pitches.
2. **`VK_EXT_image_drm_format_modifier`** lets Vulkan create an image whose
   tiling is *told to it* (`VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT` +
   `VkImageDrmFormatModifierExplicitCreateInfoEXT` with those exact plane
   layouts) instead of guessed. This is precisely the information gap that
   broke NV12 on Windows — here the chroma plane offset is explicit.
3. **dma-buf fds are kernel-refcounted**: the renderer's import keeps the
   memory alive regardless of what the decoder process does afterwards, and
   fds cross the process boundary natively via `SCM_RIGHTS`.

So the Linux pipeline is: NVDEC/media-engine writes a decode surface → that
exact allocation is sampled by the renderer. Zero copies *and* zero
conversions.

**Two sampling modes**, chosen automatically on the first imported surface:

- **Combined** (Mesa/Intel/AMD): one multi-planar `G8_B8R8_2PLANE_420_UNORM`
  VkImage, NV12→RGB inside the sampler via `VkSamplerYcbcrConversion`.
- **Planar** (NVIDIA): NVIDIA's Vulkan driver only samples multi-planar NV12
  with the LINEAR modifier, but the surfaces export with block-linear
  modifiers (`0x03000000006060xx`) — which it *does* support for single-plane
  `R8`/`R8G8`. So the renderer imports luma as an R8 image and chroma as an
  R8G8 image (each plane's own dma-buf object + explicit layout) and does the
  YUV→RGB matrix in the fragment shader. Same zero-copy property, verified
  empirically (see §7). The mode decision is a runtime query of
  `VkDrmFormatModifierPropertiesListEXT` against the exported modifier.

### Surface lifetime (the part that makes zero-copy safe)

FFmpeg's VAAPI decoder allocates a fixed pool of surfaces at codec-open time
and recycles a surface as soon as its `AVFrame` refcount drops. The decoder
therefore **holds an `av_frame_clone` ref for every frame announced to the
renderer** and releases it only when the renderer's `FrameConsumed{V}` message
promises frames with `readyValue ≤ V` will never be read again. The held ring
is capped (`--pool-size`, default 4): when the renderer stalls, new frames are
dropped rather than queued, keeping latency bounded — identical policy to
Windows. `avctx->extra_hw_frames = cap + 2` gives the codec pool headroom for
the held frames.

### Synchronization model

There are no cross-process semaphores in the Linux path (yet):

- **Producer side**: `vaSyncSurface` before `FrameReady` — a CPU wait in the
  *decoder* process guaranteeing the decode write completed. Cost: ~the tail
  of one frame's decode (typically < 2 ms, overlapped with the next
  `av_read_frame`); it never blocks the renderer.
- **Consumer side**: the renderer records the consumed value against the
  in-flight fence of the first submission that samples a *newer* frame. When
  that fence retires (proving all older sampling work retired, since a queue's
  submissions retire in order), it sends `FrameConsumed` over the socket.

The planned upgrade is fully explicit GPU-side sync:
`vaExportSurfaceHandle` + `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` on the producer and
`VK_KHR_external_semaphore_fd` (`SYNC_FD`) imports on the consumer — tracked in
ROADMAP.md. The current scheme is correct without it; it just costs the
producer-side CPU sync.

### Message flow (Linux specifics)

```
renderer                       decoder
   | ---- Hello {pid} ---------->|            (LUID is Windows-only; GPU choice
   |                             |             on Linux is --device render node)
   | <--- StreamStatus ----------|
   | <--- PoolDesc {w,h,NV12,...}|            stream header only - no handles
   | <--- SurfaceDesc {key=S1,   |            once per distinct VASurfaceID,
   |        modifier, planes[]}  |            dma-buf fd as SCM_RIGHTS payload
   |        + fd                 |
   | <--- FrameReady {S1, v=1}   |            after vaSyncSurface
   | <--- SurfaceDesc {S2} + fd  |
   | <--- FrameReady {S2, v=2}   |
   | ---- FrameConsumed {v=1} -->|            decoder drops its AVFrame ref;
   |      ...                    |            surface S1 may be decoded into again
```

A `StreamReset` precedes any event that invalidates the surfaces (resolution
change, RTSP reconnect — the codec pool is rebuilt); the renderer drops all
imports. Kernel refcounting makes the window between "decoder freed its pool"
and "renderer processed StreamReset" safe: the renderer's imports keep the
memory alive; it merely stops receiving updates.

## 2. Hardware / driver support matrix

| GPU | VAAPI driver | Package | Status |
|---|---|---|---|
| Intel (Gen9+/Arc) | iHD (media-driver) | `intel-media-va-driver` (or `-non-free` for full codec set) | The canonical path. Mesa ANV imports the exported modifiers directly. |
| AMD (GCN4+/RDNA) | Mesa radeonsi | `mesa-va-drivers` | Works with RADV. AMD exports mostly linear or vendor modifiers; both understood by RADV. |
| NVIDIA (proprietary) | [nvidia-vaapi-driver](https://github.com/elFarto/nvidia-vaapi-driver) (NVDEC shim) | not packaged in Ubuntu — built from source (see §4) | Decode-only shim over NVDEC; exports dma-bufs with NVIDIA block-linear modifiers. Renderer side needs driver ≥ ~545 for `VK_EXT_image_drm_format_modifier` + dma-buf import. |
| NVIDIA (nouveau/NVK) | none usable for decode | — | Not supported. |

Requirements common to all: kernel with DRM render nodes (`/dev/dri/renderD*`),
libva ≥ 2.x (API 1.16+), Vulkan 1.3 driver exposing
`VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`,
`VK_EXT_image_drm_format_modifier`, `VK_EXT_queue_family_foreign`, and the
`samplerYcbcrConversion` feature.

Quick capability checks:

```bash
vainfo                                  # VAAPI driver loads + codec profiles
vulkaninfo | grep -E "dma_buf|drm_format_modifier|queue_family_foreign"
ls -l /dev/dri/                         # render node present, user in 'render'/'video' group
```

## 3. Building

### Distro packages (Debian/Ubuntu)

```bash
sudo apt install cmake g++ pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev \
  libva-dev libdrm-dev \
  xorg-dev libwayland-dev libxkbcommon-dev wayland-protocols
```

FFmpeg ≥ 6.1 and libva ≥ 2.16 are required (both satisfied by Ubuntu 24.04).
Vulkan-Headers, volk, GLFW and glslang are fetched by CMake — no Vulkan SDK
needed. Fedora equivalents: `ffmpeg-free-devel libva-devel libdrm-devel
wayland-devel libxkbcommon-devel` + the X11 devel group.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build          # runs vkr_ipc_test (no GPU needed)
```

### No sudo? Home-prefix bootstrap

Everything can live under `~/.local` (this is exactly how the reference
verification box was set up):

```bash
# libva
git clone https://github.com/intel/libva.git && cd libva
meson setup build --prefix=$HOME/.local --libdir=lib && ninja -C build install

# then configure the project with:
export PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

At runtime add `export LD_LIBRARY_PATH=$HOME/.local/lib`.

## 4. NVIDIA specifics (nvidia-vaapi-driver)

NVIDIA's proprietary driver has no native VAAPI. The shim used by Firefox for
zero-copy decode works here too:

```bash
# deps: meson, libva (above), nv-codec-headers, gstreamer codecparsers
git clone https://github.com/FFmpeg/nv-codec-headers.git
make -C nv-codec-headers install PREFIX=$HOME/.local

git clone https://github.com/elFarto/nvidia-vaapi-driver.git
cd nvidia-vaapi-driver
PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig meson setup build --prefix=$HOME/.local --libdir=lib
ninja -C build install     # -> ~/.local/lib/dri/nvidia_drv_video.so
```

Runtime environment for both `vainfo` and `vkdecoder`:

```bash
export LIBVA_DRIVERS_PATH=$HOME/.local/lib/dri
export LIBVA_DRIVER_NAME=nvidia
export NVD_BACKEND=direct        # NVDEC via /dev/nvidia*, no EGL needed (headless-safe)
export LD_LIBRARY_PATH=$HOME/.local/lib
```

Notes:
- The shim is **decode-only** (fine: this project never encodes).
- `NVD_LOG=1` prints the shim's own diagnostics.
- Exported surfaces carry NVIDIA block-linear modifiers; the Vulkan side of
  the same driver understands them. Mixing GPUs (decode on NVIDIA, render on
  another vendor) will generally *not* work — modifiers are vendor-specific.

## 5. Running

Same binaries and flags as Windows, plus `--device`:

```bash
./vkdecoder --url rtsp://camera:8554/stream [--device /dev/dri/renderD128] [--pool-size 4]
./vkrenderer [--pacing latest|pts]
```

`--device` selects the DRM render node (= GPU) for VAAPI; the renderer picks
its Vulkan device independently (discrete preferred). On multi-GPU boxes make
them match — there is no LUID equivalent on Linux, so this is manual.

The IPC endpoint is a Unix socket at `$XDG_RUNTIME_DIR/vkrender-<name>.sock`
(`/tmp` fallback), so both processes must run in the same user session unless
you point `--pipe` at a name whose directory both can reach.

### Headless verification (no display session, e.g. over SSH)

The renderer has a windowless mode that renders offscreen and dumps a frame,
made for verifying GPU boxes you only reach by SSH and for CI with GPU
runners:

```bash
./vkrenderer --headless 60 --dump /tmp/frame.ppm
```

waits for 60 decoded video frames (60 s timeout), writes the last one as a
binary PPM, and exits 0 on success / 2 if the frames never arrived. View the
PPM anywhere (`ffmpeg -i frame.ppm frame.png`, GIMP, etc.).

### Full local loop test (no camera needed)

```bash
# 1. RTSP server
./mediamtx &
# 2. publisher: 1080p30 H.264 test pattern (system ffmpeg CLI)
ffmpeg -re -f lavfi -i testsrc2=size=1920x1080:rate=30 \
       -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
       -f rtsp rtsp://127.0.0.1:8554/test &
# 3. decoder (with the VAAPI env from §4 if NVIDIA)
./vkdecoder --url rtsp://127.0.0.1:8554/test &
# 4. renderer
./vkrenderer --headless 60 --dump /tmp/frame.ppm   # or windowed if you have a display
```

Expected log lines proving the zero-copy path:
- decoder: `VAAPI display ready`, `stream header sent: 1920x1080 NV12`
- renderer: `imported dma-buf surface key=N modifier=0x...` (one per distinct
  decode surface, typically pool-size + DPB count) and no further imports
  after the pool warmed up — the same surfaces cycle forever
- renderer (headless): `headless OK, 60 video frames` and a correct test
  pattern in the PPM

## 6. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `av_hwdevice_ctx_create(VAAPI...) failed` | No VAAPI driver for this GPU, or no DRM node access. Run `vainfo`; set `LIBVA_DRIVERS_PATH`/`LIBVA_DRIVER_NAME`; **check `/dev/dri/renderD*` permissions — SSH sessions don't get the desktop seat ACL, the user must be in the `render` group** (`sudo usermod -aG render,video $USER`, then re-login). Hit on the reference box. |
| `vaExportSurfaceHandle ... invalid VASurfaceID` | nvidia-vaapi-driver rejects `COMPOSED_LAYERS` with this (misleading) error — the project always uses `SEPARATE_LAYERS`, so if you see this you're running a pre-M6 build. |
| `vaExportSurfaceHandle ... failed` (other) | Driver lacks `DRM_PRIME_2` export (very old Mesa/iHD, or shim misconfig). For nvidia-vaapi-driver ensure `NVD_BACKEND=direct` and driver ≥ 525. |
| `vkCreateImage(dma-buf, modifier=...) failed` | The Vulkan driver can't create *any* image with that modifier — usually a cross-vendor decode/render mix, or NVIDIA driver too old for `VK_EXT_image_drm_format_modifier`. (NVIDIA's inability to sample it as *multi-planar* NV12 is handled automatically by planar mode and is not an error.) |
| `bad dma-buf topology` | More than 4 buffer objects/planes exported; file an issue with `vainfo` + GPU details. |
| Renderer: `no Vulkan 1.3 device with the required interop extensions` | `vulkaninfo` will show what's missing; NVIDIA needs a modern proprietary driver, Mesa needs ≥ 22.x. |
| `Could not get 'vkCreateInstance' ... ICD libGLX_nvidia.so.0` | NVIDIA kernel module ≠ userspace version (typically after a driver upgrade without reboot). `cat /proc/driver/nvidia/version` vs `nvidia-smi`; reboot. Hit this on the reference box. |
| Wrong colors (green/purple) | YCbCr conversion mismatch — check the decoder log's matrix/range against the stream; file an issue with both. |
| Corrupted/tiled-looking image | Modifier mismatch: the driver accepted the import but sampled with the wrong layout. Set `NVD_LOG=1` (NVIDIA) and compare the exported modifier against `vulkaninfo`'s `drmFormatModifier` list for `G8_B8R8_2PLANE_420_UNORM`. |
| Decoder log: `N frames outstanding, dropped ...` | Renderer not consuming (stalled, or `FrameConsumed` not arriving). Benign if transient; persistent means the renderer died non-gracefully — it will be caught by the socket close. |

## 7. Verification status (updated as tested)

| Item | Where | Status |
|---|---|---|
| Build (all targets) | Ubuntu 24.04 x86_64 (melchior), GitHub Actions ubuntu-latest | ✅ builds clean |
| `vkr_ipc_test` (UDS framing, SCM_RIGHTS fd passing, peer pid, disconnect) | melchior + CI | ✅ passes |
| **Full pipeline: RTSP → NVDEC (nvidia-vaapi-driver) → dma-buf export (block-linear modifier 0x300000000606014, 2 objects) → planar R8/R8G8 Vulkan import → headless render → frame dump** | melchior: RTX 3090, driver 580.173.02, Ubuntu 24.04, H.264 1080p30 | ✅ **verified 2026-08-03** — 60 frames, pixel-correct test pattern, 3 decode surfaces cycling zero-copy, 1 warm-up drop |
| Combined-mode NV12 import (`VkSamplerYcbcrConversion`) | — | implemented; needs Intel iHD or AMD radeonsi hardware to exercise (§5 procedure applies verbatim) |
| Windowed renderer on X11/Wayland | — | code path identical to the Windows GLFW loop; needs a desktop session to exercise |
| Explicit GPU-side sync (sync_file) | — | not implemented; producer-side `vaSyncSurface` in use (see §1) |
