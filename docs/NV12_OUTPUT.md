# NV12 FrameLink scene output (Windows)

The Windows `--serve` renderer now follows the consumer-owned scene channel's
format: BGRA keeps the existing scene blit; NV12 produces BT.709 limited-range
4:2:0 output using the Windows GPU video processor. Requires FrameLink protocol
v4 with Windows NV12 support (`jsuppe/framelink`, `feature/nv12`, commit 0921163
or later). Existing receiver/source checkouts are not silently replaced.

## Use

Create the `<service-name>.scene` channel as `FL_FORMAT_NV12` with positive even
dimensions on the same adapter as the renderer. Start the renderer with
`vkrenderer --serve <service-name>` (optionally `--headless -1`). It attaches when
the consumer exists and publishes while an input is actively presenting and
camera output is enabled. A BGRA consumer continues to receive BGRA.

GStreamer example (updated FrameLink plugin/runtime on PATH/GST_PLUGIN_PATH):

```sh
gst-launch-1.0 framelinksrc channel=vkrender.scene format=NV12 width=1920 height=1080 \
  ! video/x-raw,colorimetry=bt709,chroma-site=mpeg2 \
  ! videoconvert ! autovideosink
```

FrameLink does not transport colorimetry metadata. The receiving application
must use BT.709 limited-range YUV, left-sited chroma. A consumer running on a
different GPU is not supported by this path.

## GPU ordering and copies

1. Vulkan renders the scene normally and blits/scales it into a per-slot shared
   BGRA intermediate.
2. That Vulkan submit signals the intermediate's imported timeline fence.
3. FrameLink's D3D11 context queues a GPU fence wait, then video-processor
   conversion into the actual NV12 FrameLink slot.
4. `flSubmit` signals FrameLink's ready fence on that same D3D11 context, after
   conversion. The consumer sees readiness only after both planes are written.
5. FrameLink's consumed fence controls reuse. Teardown waits for outstanding
   conversion before freeing intermediate resources.

No CPU pixel mapping/readback is used by the renderer. This is not a zero-copy
or direct-NV12-color-attachment claim: an RGB intermediate and GPU conversion
remain. The GStreamer consumer may additionally perform its usual CPU readback.
The driver must support BGRA input -> NV12 output with the specified color spaces;
initialization checks this and declines publication if unavailable.

## Why not write the imported NV12 image directly from Vulkan?

An initial compute path correctly produced both planes in a GPU buffer and
copied them into an imported NV12 VkImage. On Caspar's RTX 5090, Vulkan readback
of that image showed correct Y/UV, while D3D11 readback of the shared NV12 texture
showed correct Y but zero UV. This reproduced at 64x32, 130x66 and 1920x1080,
including after waiting for the GPU. Import capability queries reported support.
That path was not shipped. The tested Windows video-processor path avoids this
observed interop failure. This is an observation on this system, not a conclusion
that all Vulkan drivers cannot write shared NV12 images.

## Tests

`vkr_nv12_output_test` renders known RGB colors with Vulkan, performs the same
shared-intermediate/fence/video-processor path, and verifies every Y/UV byte
through FrameLink's D3D11 consumer. Tests allow one code-value rounding error.
The 64x32 test additionally renders a per-pixel 2x2 color checkerboard, verifying
luma placement and chroma downsampling. There are 24 received frames per test.
130x66 deliberately exercises padded row pitches; 1920x1080 tests HD output.

```sh
cmake --build build --config Release --target vkrenderer vkr_nv12_output_test
ctest --test-dir build -C Release -R nv12_gpu --output-on-failure
```

`test/nv12_service.ps1` exercises the actual service with three separate processes:
a FrameLink input producer, the renderer, and a GStreamer scene consumer. It checks
30 output frames, output byte count including GStreamer row padding, changing
frame identities and (NV12) non-neutral chroma. It stops only its own processes.
Build FrameLink's tests/plugin first so `fl_produce.exe` and `gstframelink.dll`
are available. Run once for each format:

```powershell
./test/nv12_service.ps1 -RendererBuild ./build -FrameLinkBuild ../framelink/build -Format NV12
./test/nv12_service.ps1 -RendererBuild ./build -FrameLinkBuild ../framelink/build -Format BGRA
```

Qualification host: `jonsu@caspar`, NVIDIA RTX 5090, VS2022 x64 Release.
The isolated checkout/build is `C:\Users\jonsu\dev\vkrender-nv12-20261029`.
These are correctness/integration tests, not a frame-rate benchmark. The Linux
Wayland frontend does not gain a new scene-output channel in this change.

Verified 2026-10-29: all three GPU tests passed (1.30 seconds total).
Full-service captures passed for NV12 (30/30 distinct frames, 30/30 with
non-neutral chroma, 392040 bytes) and BGRA (30/30 distinct, 1029600 bytes).
