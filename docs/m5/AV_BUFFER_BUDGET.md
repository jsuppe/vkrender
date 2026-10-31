# A/V transport ownership budget

The A/V receiver now requests **12 shared buffers**, independently of its
**six-frame timed queue**. Original PTS, selected clock, common A/V latency,
late-frame threshold and default non-A/V channel allocation are unchanged.

## Why eight was insufficient

[Recorded Windows ownership](ring-ownership-20261031/README.md) found all eight
buffers occupied by six future frames, one current image and one earlier image
still protected by the GPU consumed timeline. Other failures held five future
frames, current and two older images. No producer-write slot remained.

The operating envelope counts distinct identities, not references or repeated
draws: six pending, one current, two older GPU-flight images, one completed-read
image awaiting its ordered retirement signal, one IPC mailbox and one writer.
Twelve is conservative normal-cadence headroom, **not** a proof of losslessness
under arbitrary stalls or latest-wins burst replacement. Runtime validation is
required. Releasing a still-read image early would be incorrect; adding a CPU
wait or extra GPU copy would change the performance path.

## Compatibility and memory

- Requires the coordinated FrameLink larger-pool protocol update. Rebuild the
  producer, consumer, GStreamer plugin and renderer against the matching version;
  mismatched peers must reject before exchanging native handles.
- FrameLink-facing arrays use `FL_MAX_POOL_SIZE`. The renderer's separate legacy
  protocol stays unchanged at eight; no accidental legacy wire change is made.
- Windows pool keys reserve four slot bits; a compile-time guard rejects a future
  FrameLink limit above16. The GPU-flight constant is checked against the budget.
- At1080p the four extra shared images add approximately31.6MiB for BGRA or
  11.9MiB for NV12 payloads, excluding driver alignment, staging and renderer-owned
  resources. This is allocation headroom, not added scheduled playback delay.

## Controlled comparison

`VKR_TEST_AV_POOL_SLOTS=8` selects the former capacity; `12` or unset selects the
new budget. Other values are rejected. This test-only override changes storage
only; `AV_RING_CONFIG` records pool, pending limit and common latency.

Compare both capacities with the same executable/plugin, source identities,
format, clock,100ms latency and output mode. Repeat across startup phase; count
startup, steady and drain frames. A complete accounting report with drops is a
failed full-delivery result. Confirm pixels, A/V offsets, safe delayed-GPU release,
reconnect and trace-off/on behavior before claiming the performance gate passes.
