# GPU-only FrameLink scene output (2026-10-29)

Status: implemented and hardware-qualified as below, **not universal NV12 GPU support**.
Changes are on `feature/platform-parity`; this document does not imply master includes them.

## Enable explicitly

Desktop Linux: set `VKR_OUTPUT_MODE=gpu` and supply repeatable
`--scene-output channel` options. Default remains the existing `cpu` fallback.
Android: restart the renderer with `--es outputMode gpu` and
`--es sceneOutputs output.rgba,output.nv12`.
The consumer creates the channels first. Pass `FL_GPU_RENDER` to
`flCreateChannelEx`; combine with `FL_MAP_CPU` only when CPU inspection is needed.
The consumer sample exposes `--gpu-render 1`. Android checks allocation support
and returns `FL_FORMAT_UNSUPPORTED` rather than silently changing the format.

GPU mode is strict: an unsupported writable import is logged and refused, **not
silently replaced with a CPU conversion**. Existing CPU mode remains available.

## Data path and ownership

- Scene image -> device buffer -> compute conversion -> shared DMA-BUF image on Linux.
- Android RGB: same GPU compute/copy path into a renderable AHardwareBuffer.
- Android native YUV: fragment shader produces limited-range BT.709 Cr/Y/Cb,
  then `VK_ANDROID_external_format_resolve` writes the external-format AHB.
  Chroma is pre-box-filtered over each 2x2 output block and replicated across
  that block, so driver resolve selection/averaging does not change the filter.
- Both RGB and YUV sample the same source coordinates, including when scaling.
- Native images and intermediate device buffers are reused; no pixel map,
  readback to the CPU, CPU color conversion, or extra queue/device-idle wait.
- The caller's existing frame fence retires each submission. Only then is its
  frame published through `flSubmitSyncFd(...,-1,pts)` (already complete).
  This is **completion-gated publication**, not an early exported ready-fence
  path. It retains the existing frame-ring latency. CPU control/API calls remain.
- Recording is not publication: only successful queue submissions are armed
  through `submitted()`. Disconnected channel storage is retained until all
  outstanding frame slots retire. Recreation discards old imports.

This is GPU-only pixel processing, **not zero-copy or direct-to-NV12 scene shading**.
The intermediate GPU copies remain. NV12 uses a native multi-plane destination,
not a normal color attachment on Linux. Separate R8/RG8 import aliases were
rejected during validation because dedicated imports did not preserve plane offsets.

Android `FL_FORMAT_NV12` retains the existing contract: native YUV420 AHB storage,
with a normalized NV12 CPU mapping if its native layout is not NV12. GPU consumers
import the native AHB. Consumer CPU mapping may normalize planes; that does not
make an entire CPU-reading consumer pipeline GPU-only.

## Verified devices and formats

- Linux RTX 3090: BGRA8, RGBA8, RGB10A2 and NV12.
- Android Mali-G52 tablet: all four formats, including native external-format YUV resolve.
- Android Pi V3D 7.1: BGRA8 and RGBA8.
- Pi NV12 GPU-renderable allocation is unsupported and external-format resolve
  is absent. Pi RGB10A2 allocation is also unsupported. CPU NV12 remains available.
- Windows existing D3D/Vulkan GPU output path is unchanged.

Linux import currently accepts shared-object DMA-BUF plane layouts. Unsupported
modifiers/disjoint objects are rejected. Device capability checks still matter.
Android defined-format YUV AHB imports are explicitly refused: the current Android NV12 GPU path requires external-format resolve, not the single-plane RGB copy path.
The Android null-color-attachment resolve branch was hardware-tested on Mali;
the non-null intermediate-color-attachment branch is implemented but unverified
on hardware requiring it.

To support GPU-only native NV12 on this Pi would require driver/gralloc support
or a new transport representation (e.g. separately shared Y and UV images), not
pretending its current native YUV AHB can be written through Vulkan.

## Tests and evidence

- `vkr_gpu_output_caps`: device extensions, format features and Android allocation probe.
- `vkr_gpu_output_pixels <1|2|3|4>`: 24 changing patterned GPU-output attempts;
  23 frames verified, one intentionally discarded when the consumer dies while
  output is pending. Recreates at 134x70 from 130x66, checks all mapped pixels,
  chroma/stride handling and absence of publication before submission/retirement.
  A recorded-but-never-submitted output is discarded at shutdown.
- A `large` test argument exercises 3840x2160 -> 3844x2164 output, including
  two-dimensional compute dispatch beyond the 65,535-workgroup X limit.
  Linux RGBA, Pi RGBA and tablet NV12 passed with zero byte error.
- Linux all four formats passed with zero byte error; tablet all four passed;
  Pi BGRA/RGBA passed. Unsupported Pi formats skip, not pass.
- `python3 test/gpu_output_integration.py BUILD`: live scene RGB/NV12 comparison;
  Linux 20 timestamp-matched distinct pairs passed, maximum YUV error 1 (limit 3).
- Linux full renderer suite: 9/9; FrameLink: 25/25.
- Linux NV12 lifecycle test ran with `VK_LAYER_KHRONOS_validation`, no reported errors. The 4K Linux RGBA lifecycle test also ran with validation
  enabled without reported errors.
- Tablet native patterned tests passed. The full visible Activity comparison
  could not run while its lock screen paused the Activity; not counted as passed.

Regenerate embedded shaders after edits using `python3 test/update_output_shaders.py`.
The checked-in SPIR-V allows Android builds without a host shader compiler.

## Timing and tracing

`VKR_TRACE=/path/prefix VKR_TRACE_GPU=1` includes `output.gpu_record`,
`output.gpu_publish` CPU scopes and per-output `gpu.output_duration` GPU timestamps.
The GPU records carry the producer channel handle as object ID and source PTS
as sequence. Query collection occurs only after the existing fence, without a
profiling wait. No timestamp queries are created with GPU tracing disabled.
A traced lifecycle test produced 24 GPU duration records (including the output
intentionally discarded during consumer recreation), with no pixel errors.

Linux Release build, 1280x720 background scene -> 1920x1080 linear NV12,
consumer verification/mapping disabled, trace enabled, 20 delivered frames/run:

- CPU runs: 22.620 / 22.220 seconds; 0.88 / 0.90 received fps;
  mean PTS age 2181.7 / 2147.1 ms.
- GPU runs: 0.580 / 0.579 seconds; 34.61 / 34.66 received fps;
  mean PTS age 34.1 / 34.3 ms.

These short runs **include startup**; they are not sustained 1080p-rate,
scanout-latency or thermal qualifications. An initial 120-frame CPU run exceeded
its 60-second harness limit; the bounded comparison used 20 frames on both paths.
Raw logs are in `docs/gpu-output-20261029/`. Long-running stress and additional
hardware/driver coverage remain separate qualification work.

The first 4K validation attempt timed out while repeatedly reading individual
bytes from uncached mapped GPU memory. The checker now copies each mapped row
once into CPU-owned verification storage; the unchanged GPU path passes in
3.6 seconds on Linux. This is test-only readback, not the renderer output path.

## Sustained follow-up

See [Linux stress evidence](linux_gpu_stress_20261029/README.md) for the 18,000-pair run and process-crash/resize cycles. Arrival gaps are reported rather than treated as a drop-free pass.
