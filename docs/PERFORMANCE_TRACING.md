# Performance audit and tracing

Requirement: opt-in per-frame evidence for the renderer and FrameLink without
synchronous hot-path logging, hidden capture loss, or CPU/GPU timing confusion.
Zero enabled overhead is impossible. Disabled recording is a cached branch;
`VKR_DISABLE_TRACE=ON` compiles out recording. Existing human-readable logs are
unchanged. This change is not a promise to measure every hardware counter.

## Audit

Previously `VKR_SCENE_TIMING` sampled CPU phase times and the service printed
coarse throughput/adoption averages. Those are insufficient to distinguish
producer pacing, IPC delivery, input fences, output backpressure, command
recording, GPU execution and presentation. Printing each frame to stdout would
add scheduler/I/O jitter precisely where it is being investigated.

New coverage:

- `service.iteration`, `service.headless_pacing`: entire loop and intentional sleep.
- `service.output_prepare` / `service.output_publish`: independently attributed
  to each output channel (same FNV-1a channel ID as FrameLink).
- `scene.frame_cpu`, `scene.record_commands_cpu`: CPU elapsed time, not GPU time.
- `scene.wait_inflight_fence`: existing CPU fence wait; result is Vulkan status.
- `scene.acquire_swapchain`, `scene.queue_submit_cpu`, `scene.present_call_cpu`:
  CPU driver/API duration and result, **not actual display presentation**.
- `gpu.scene_duration`: optional Vulkan top-to-bottom timestamp interval,
  including scene rendering and output blits (not isolated shader time).
  Timestamp valid-bit wrap and device timestampPeriod are handled. Results are
  read after the existing per-frame fence; no WAIT flag or extra GPU wait.
  The last in-flight results are collected at normal teardown after its existing
  device-idle wait. `start_ns` is the CPU observation time, **not GPU start time**.
- `nv12.queue_conversion_cpu`: CPU cost to enqueue GPU wait, video processing,
  completion signal and flush. Boolean result (1 success, 0 failure).
- `nv12.fence_observation`: next slot-use observation of the previous conversion
  fence. `sequence` is completed fence value, `fence` requested value, `pts_ns`
  is prior enqueue timestamp; result 0=complete, 1=not yet complete, -1=device removed. It has zero
  duration intentionally. Observation minus enqueue is only an **upper bound**
  on completion latency, including time until the next observation.
- FrameLink/GStreamer tracing supplies transport, frame identity, replacement,
  CPU mapping/readback and end-to-end API delivery evidence; see that repository's
  `docs/PERFORMANCE_TRACING.md` and `tools/trace_report.py`.

The scopes overlap: never sum parent and child spans. Scene sequence is a render
iteration ID; FrameLink sequence is consumer-side arrival count. They are NOT the
same counter. Match transport frames by channel/fence/PTS, not sequence alone.

## Enable

Use a matched FrameLink checkout containing `include/framelink_trace.hpp`.
Prefixes must point to existing directories and be different per component.
Environment is read once; set it **before starting the process**.

```powershell
$env:VKR_TRACE='C:\captures\renderer'
$env:VKR_TRACE_GPU='1' # optional GPU queries; omit for CPU-only tracing
$env:FRAMELINK_TRACE='C:\captures\transport'
$env:FRAMELINK_GST_TRACE='C:\captures\gstreamer'
$env:VKR_TRACE_EVENTS='262144' # per-thread event budget
# Start the renderer normally; exit gracefully to save <prefix>.<pid>.csv.
```

Each recording module has its own bounded per-thread append-only buffers; no
per-frame formatting, disk writes or shared recorder mutex after registration.
There is first-use allocation and thread registration. Full buffers stop
recording new events and count drops; they do not wait or overwrite old data.
The default is 65,536 events/thread (about 4.5 MiB), up to 64 participating threads.
See the loss footer before trusting any rate/distribution. A partial capture is
not an exhaustive trace. GPU queries are separately opt-in because they alter
command streams and their overhead is not covered by the CPU recorder benchmark.

Normal process exit exports CSV. Force-killing the process loses the in-memory
capture. `vkrtrace::flush()` allows a control-thread snapshot in an embedded
integration; do not put it in the render loop. Export performs synchronous I/O
on the calling thread and can contend with application I/O, so do it outside the
measurement window. It snapshots without resetting or replenishing capacity.

## Reproducible dual-output trace test (Windows)

```powershell
./examples/framelink_compare.ps1 -RendererBuild ./build `
  -FrameLinkBuild ../framelink/build -CaptureTrace
```

This checks 60 received frames and at least 30 distinct images in each RGB and NV12→RGB path,
runs the renderer for 90 adopted input frames, waits for normal renderer exit,
and checks CPU-submit events, GPU-duration events and zero recorder loss. The
input producer is then stopped; its forced-stop tail is not guaranteed to be
exported. Receiver and renderer traces are exported on normal exit. A separate
FrameLink finite producer/consumer sample run can capture both transport ends.
GPU-timestamp support is required by this test; the renderer itself gracefully
records `gpu.scene_unavailable` when unsupported.

```sh
python ../framelink/tools/trace_report.py 'build/framelink-compare-*/*.csv'
```

## Precision and limits

- Nanosecond units do not imply nanosecond clock resolution or accuracy. CPU
  spans include preemption, blocking and scheduler delay. They are not CPU cycles.
- GPU timestamp records cannot be placed on the CPU timeline without calibrated
  timestamps. They describe duration, not a calibrated render-to-screen latency.
- Experimental D3D11 timestamp queries bracketing VideoProcessorBlt on Caspar's
  RTX 5090 returned 0–32 ns intervals, which did not measure video-engine work.
  That implementation was removed. Use ETW/GPUView/vendor tools to isolate the
  video engine; fence observations must not be relabeled conversion duration.
- Swapchain present-call return does not mean scanout. Windows presentation,
  compositor queuing, missed vblanks and scanout require ETW/PresentMon and/or
  external display capture. This patch does not infer them from callback rate.
- OS scheduling, GPU occupancy/cache/bandwidth/power/thermal counters, decoder and
  network internals are outside these application traces. Use system profilers
  alongside them. Android and the Linux Wayland frontend were not qualified in
  this Windows renderer validation; shared scene instrumentation is source-level
  coverage, not a claim that all frontends have been benchmarked.
- Any new geometry/import failures remain visible in existing diagnostic logs.
  Capture logs alongside CSV; opaque image pointers in FrameLink map events are
  process-local identities, not cross-process channel IDs.

## Validation

2026-10-29: FrameLink Linux 17/17 and Windows 21/21 regression tests passed;
trace-enabled transport/NV12/slow-consumer/reconnect tests passed. Vulkan NV12
pixel tests 3/3 passed on Caspar, and the dual-output capture delivered 60/60
changing frames on both channels. See FrameLink's tracing document for recorder
microbenchmarks; those are not whole-pipeline or GPU-query overhead estimates.

Final captured example: 295 Vulkan GPU intervals, 60 NV12 fence observations,
120 unambiguous output submit/acquire joins, zero dropped recorder events.
All 60 images in each output were distinct. The report observed ~0.977 ms
median submit-entry-to-acquire-return latency in this particular run; this is
a correctness demonstration under concurrent desktop load, not a controlled
performance or logging-overhead qualification.

## Android / portable parity extension

The Android renderer now uses the same bounded CPU recorder and Vulkan query
helper. Intent extras `trace`, `traceGpu`, and `traceFrames` configure a finite
capture before native initialization; files are written to the app's private
files directory and flushed on normal teardown. `sceneOutputs` enables the
portable asynchronous readback/CPU-conversion path. Its CPU conversion spans
are not GPU conversion durations. See `PLATFORM_PARITY.md` for device tests.
