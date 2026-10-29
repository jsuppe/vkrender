# Windows GPU stress, 2026-10-29

Host: Caspar, NVIDIA GeForce RTX 5090. Isolated `build-trace/Release` builds;
existing interactive `build/Release` demo was not modified or stopped.
The pre-existing demo may compete for GPU resources: these are operational
stress observations, not exclusive-device benchmark numbers.

## Methods

- `test/windows_gpu_stress.ps1`: native GPU scene output to independent BGRA/NV12
  channels, consumer-owned GPU fences, no CPU pixel mapping or verification.
  Input is GStreamer SMPTE 1280x720 at 30 fps; the renderer scene is animated at its
  own nominal 60 fps. Outputs 1920x1080 except explicit resize phases.
- `test/windows_gpu_stress_pixels.ps1`: separate pixel-readback comparison of
  timestamp-matched RGBA/NV12 160x90 outputs, limited BT.709 tolerance 3. This is a
  correctness/pacing workload, not a no-readback throughput benchmark.
- `ctest -R 'disconnect_full_ring|ready_fence_timeout|reconnect_held'
  --repeat until-fail:20`: 60 regression invocations. The held-frame test gives
  deterministic ownership coverage; arbitrary process-crash timing does not.
- Process private bytes, working set and handle count sampled about once/sec.
  These are CPU process metrics, **not dedicated GPU memory residency**.
- Acquisition PTS age and interarrival timings are **not scanout latency**.
  Consumer fps includes startup; gaps count transport sequence IDs, not unseen
  producer render attempts. No claim of zero dropped frames or unique pixels
  is made from metadata alone.

## Initial run: `gpu-stress-23384`

- Sustained 1080p: 3,600 frames per channel, 0 sequence gaps; 53.73 fps including startup.
- PTS age mean/max: BGRA 1.501/4.372 ms; NV12 1.507/13.155 ms.
- Resize 720p with 2-second input-process interruption, resize 1080p, forced consumer
  termination/recreation, and renderer restart with new consumers: 300 frames per
  channel per phase, all passed, 0 sequence gaps.
- Total 4,800 frames/channel across completed counted phases. Abruptly terminated
  consumers also ran but their partial counts are not included.
- After 15 s startup, sustained private memory 361.67→362.61 MiB, max 362.67 MiB over
  roughly 52 s. This short observation does not establish absence of leaks.
- Longer run and surviving-consumer restart are documented below when complete.

`lifecycle-20x3.txt`: all 60 repeats passed.

## Sustained pixel comparison

6,000 matched pairs, 6,000 distinct RGB hashes, worst YUV error 1, zero sequence gaps
on both channels; 102.288 s including startup. Arrival timings in milliseconds:

- RGB: p50 16.739, p95 18.285, p99 18.706, max 26.790.
- NV12: p50 16.729, p95 18.315, p99 18.564, max 19.999.

Raw evidence:`parity-output-check.out` and adjacent renderer/input logs. Timings
include CPU mapping and pixel verification overhead and the already-running demo.

## Five-minute run: `gpu-stress-40072`

- Sustained 1080p: **18,000 frames per channel**, **58.73 fps** including startup,
  **zero transport sequence gaps**. PTS age mean/max: BGRA 1.506/4.016 ms;
  NV12 1.509/13.441 ms.
- After the first 15 seconds, private memory 355.516→356.699 MiB over 291 seconds;
  min/max 355.516/357.031 MiB. Handles started/ended at 2,120, range 2,114–2,123.
  No large growth observed; these measurements cannot establish leak freedom.
- Resize to 720p and back to 1080p: 300 frames/channel per phase passed.
- Input process killed, two-second gap, restarted during 720p phase: delivery
  continued/recovered and input adoptions resumed in renderer logs.
- **Same consumers survived renderer process termination and restart**: 600
  frames/channel, zero sequence gaps; 416/419 timeout/disconnect retries during
  the deliberate interruption. This is not a zero-stall guarantee.
- Abrupt consumer termination, channel recreation: 300 frames/channel passed.
- A further renderer restart with new consumers: 300 frames/channel passed.
- Total **19,800 frames/channel** in completed counted phases, all consumer exit
  codes zero. Partial intentionally killed captures are excluded.
- No CPU pixel verification in these metadata phases; the separate 6,000-pair
  pixel test establishes correctness at its own smaller output geometry.
- A separate CPU-only Windows build was permitted after the sustained phase
  completed; late lifecycle timings are not isolated benchmarks.

All harness processes were cleaned up. The pre-existing live demo remained
running. The metadata harness can be rerun with `-Frames 18000`; defaults use
that value. The pixel harness accepts `-Frames` and `-TimeoutSeconds` (defaults
6,000/240). Neither is an hours-long thermal/endurance qualification.
