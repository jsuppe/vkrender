# Renderer rate cells (1080p60 and 4K30)

FrameLink qualifies its transport at 1920x1080@60 and 3840x2160@30. The cells
here measure the renderer at those targets end to end:

```
framelink_producer ─▶ vkrenderer (headless service) ─▶ scene outputs .rgba + .nv12 ─▶ vkr_portable_output_check
  WxH @ fps, BGRA/NV12      composites at WxH                CPU or GPU output path           times every arrival
```

| Script | Platform | Renderer mode |
|---|---|---|
| `test/windows_rate_cell.ps1` | Windows | `vkrenderer --serve --headless -1` |
| `test/linux_rate_cell.py` | Linux | `vkrenderer --wayland --headless -1` |

Each run takes a cell (`1080p60`, `4k30`), an input format (BGRA, NV12), an
output mode (`gpu` = `VKR_OUTPUT_MODE=gpu`, `cpu` = the portable readback
fallback), and a duration. Qualification is `-Seconds 1800` / `--seconds 1800`.
Shorter runs are smoke checks only. Every run writes `summary.json`, the raw
process logs and the memory samples to its output directory. A failure is kept
as recorded; it is never retried into a pass.

## Gates

They follow FrameLink's primary cells (`docs/m6/PRIMARY_RESULTS.md` there):

- Output: zero arrivals over the budget (17 ms at 60 fps, 34 ms at 30 fps) and
  zero sequence gaps on both scene outputs over the measured window. The window
  opens 10 s after the first output frame.
- Input: every steady-state 5 s renderer window adopts all `fps x 5` input
  frames (one less allowed for window-edge rounding), and the renderer's
  producer-to-adoption hop maximum stays within the budget.
- Pixels, CPU output only: about one frame per second is copied out and its
  RGBA and NV12 outputs must agree within the BT.709 limit (3). GPU-output
  cells time arrivals only. Mapping GPU-rendered images stalls acquisition, and
  NVIDIA Linux maps them in a non-linear layout. Their pixels are covered by
  the `gpu_output_pixels_*` ctests.
- Memory: at most 64 KB/min private-memory growth per process (renderer,
  producer, checker), as a least-squares slope after warm-up + 30 s. Short smoke
  runs can show the checker's bounded sample buffer filling up; judge memory
  only on 1800 s runs.

The checker's duration mode (`VKR_CHECK_SECONDS`) gives each channel its own
acquire thread and verifies pixels on a third thread, so the timing measures
the renderer, not the checker. Without `VKR_CHECK_SECONDS` the checker behaves
as before.

## Running

Windows (after `cmake --preset ci && cmake --build --preset ci`; run each cell
in its own PowerShell process, under the validation mutex):

```powershell
powershell -File test\windows_rate_cell.ps1 -RendererBuild build -Cell 1080p60 -Format BGRA -OutputMode gpu -Seconds 1800
```

Linux:

```sh
python3 test/linux_rate_cell.py build/RelWithDebInfo --cell 4k30 --format bgra --output-mode gpu --seconds 1800
```

Run one cell at a time per host: a second GPU workload on the same machine
invalidates the timing.

## First results: Linux smoke, 2026-11-27

60 s each, vkrender `682cf81` (master `d4abe65` plus these tests), FrameLink
`0d72a94b`, NVIDIA desktop GPU. These are smoke results, not qualification.

| Cell | GPU output | CPU output |
|---|---|---|
| 1080p60 BGRA | Fail: 105 RGBA and 100 NV12 output arrivals over 17 ms (max 19.05 ms, p50 16.79 ms), 0 gaps; input 296/300 per 5 s | Fail: about 1.46 s per output frame |
| 1080p60 NV12 | Fail: about 2.8 s per output frame | Fail: about 4.3 s per output frame |
| 4K30 BGRA | **Pass**: max 22.9 ms against 34 ms, 0 gaps, 149/150 per 5 s, hop max 19.4 ms | Fail: about 5.8 s per output frame |
| 4K30 NV12 | Fail: about 11 s per output frame | Fail: about 5.8 s per output frame |

Findings (not fixed here):

1. **Linux NV12 FrameLink input is not imported natively.** FrameLink input
   slots import only single-plane BGRA DMA-BUFs. Every other format goes
   through `uploadFrame`, which maps the producer's GPU buffer and converts
   NV12 to BGRA on the CPU, on the render thread
   (`renderer/wayland_compositor.cpp`, `if(now.format!=FL_FORMAT_BGRA8)`).
   Stack samples during a 1080p NV12 run were 25/25 in `vkr::nv12ToBgra`. The
   SSE2 conversion is not the cost: reading the GPU-backed mapping is. Wayland
   NV12 clients already import natively, so the gap is specific to FrameLink
   slots.
2. **Linux headless pacing runs slightly below 60 Hz.** The Linux headless
   loop sleeps 16 ms and then renders (`usleep(16000)`), so its period is 16 ms
   plus the frame's work: p50 16.79 ms at 1080p. The Windows service uses the
   fixed-cadence `HeadlessPacer`. At 60 fps input this misses the 17 ms gate
   and drops some input frames. At 4K30 the 34 ms budget absorbs it.
3. **The Linux CPU output fallback is far too slow at these sizes:** about
   1.5 s per frame at 1080p BGRA. Not diagnosed yet.

Windows smoke and 1800 s runs: pending (the Windows host was running another
FrameLink validation).
