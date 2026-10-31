# Opt-in Windows software frame accounting

Enable `VKR_TRACE=path/prefix` and `FRAMELINK_TRACE=path/transport`, plus
`FRAMELINK_GST_TRACE=path/gst` (check plugin environment macro in the build).
Trace capacity is bounded; verify footer loss counts before trusting accounting.

The service emits `av.frame_schedule`, `av.frame_adopt`, and explicit
`av.frame_drop.{late,invalid,import_failed,generation_end,generation_changed}`.
The scene host correlates **actual face-slot references** against imported frame
identities, including retained tiles, rather than assuming all acquired streams
are rendered. It emits:

- `av.frame_render_submit`: successful queue submission referencing that frame.
- `av.frame_gpu_complete`: the associated submission fence completed (or device
  drained successfully at shutdown). This does not mean the frame is no longer
  retained for future scene draws or that its transport slot has been recycled.
- `av.frame_present_submit`: present API result for that scene. Not physical
  scanout, compositor latch, visibility, or proof every face covers a pixel.

For these events `channel_id` is the channel-name hash, `sequence` the acquired
transport sequence, `fence` the ready value, **`slot` the ring generation**, and
`pts_ns` original media PTS. For the last three events **`duration_ns` is the scene
submission ID**, not duration (they are instantaneous events). Repeated rendering
or presentation of one video frame intentionally produces repeated records; use
submission IDs to pair GPU completions, and count unique input identities
separately. The scene's generic timing scope records retain ordinary duration
semantics.

The FrameLink `tools/frame_accounting.py` reader supports these names. A CSV
capture must be a single generation per channel and passed with its actual
`--generation`. It rejects mismatches; reconnect traces need splitting or a
JSONL export with explicit session/generation IDs. No physical FPS claim follows
from these traces.

Validation: Linux Release compilation of shared SceneHost code and plugin passed.
Windows service instrumentation still requires the Windows worker's compile/run.
