# Windows native per-frame qualification, 2026-10-31

These are software submission/completion observations, **not physical scanout**.
See each capture's BUILD.txt, raw CSV.gz and identity accounting JSON.gz.
FrameLink b551b63; renderer c54cf7f; exact source manifests generated from the
known videotestsrc frame count/rational FPS, not from received frame identities.
Four sequential 45-second cases used native Vulkan windows, 100 ms shared A/V
latency, ring8/pending6, BGRA/NV12 at1080p30/60. Preview was off.

- BGRA30:1350 source,1348 unique successful presented identities;2 startup drops.
- NV1230:1350 source,1347 unique successful presented identities;3 startup drops.
- BGRA60:2700 source,2694 unique successful presented identities;6 startup drops.
- NV1260:2700 source,2691 unique successful presented identities;9 startup drops.
- Every case has zero steady/end identity loss. Every source identity is accounted
  for; no missing trace footers/events, no unmatched render/completion tuples.
- Startup is NOT lossless. Trace points to lazy flOpenProducer (~158 ms) inside
  the first sinkRender, after the common epoch starts, followed by catch-up.
  These captures predate the separate startup preconnect correction.

Matched45s old-retirement control (same FrameLink, tracing,100ms) has2655/2700
successful unique identities,45classified losses:8startup,37steady ring_full,
zero end. Successes in successive5s intervals:276,282,297,300,300,300,300,300,300.
Fixed-retirement NV1260:291,300,300,300,300,300,300,300,300. The original issue is
phase-sensitive retirement headroom, not an unconditional56fps cap. Both runs
are retained; no unsuccessful frames have been removed from reported totals.

## Additional correctness gate discovered

Khronos validation in a native window reports two
VUID-vkQueueSubmit-pSignalSemaphores-00067 errors both before and after the
retirement change. This is a pre-existing per-flight binary presentation
semaphore reuse bug, independent of the consumed timeline. These failing logs
are preserved in the control/retire validation folders. Correction6e2ba57 uses
one binary presentation semaphore per acquired swapchain image and rebuilds
on recreation/display changes; the following qualification folders are added
separately. A good acquisition rate was not used to waive this correctness gate.

Correction 6e2ba57 built on Linux and Windows; Linux22/22 tests passed. Native
10-second normal and two-window-resize calls both finish without validation
errors. The resize capture initially has one unclassified missing successful
present: its failed present API result is retained in raw trace; this is not
silently converted into a successful presentation. Delayed GPU stress is a
SEPARATE headless run because the gate is intentionally headless-only:96actual
40ms GPU gates,40forced import failures,0validation errors,600source identities,
488adopted/rendered/GPU-completed and112explicit failure/backpressure outcomes.
The native import-failure run with the old semaphore allocation had no actual
GPU gates and must not be used as delayed-GPU evidence.

Analyzer94a18bf resolves the preserved resize result without a rerun:600fixture
identities accounted,1189draw/present attempts (1187accepted,2OUT_OF_DATE).
PTS1.550s was rejected then successfully retried; PTS3.583333333s had only a
rejected attempt and is classified presentation failure. Thus552unique accepted
identities plus47earlier explicit drops plus1presentation failure =600. This is
complete accounting of a disrupted resize test, not lossless delivery.

## Final combined preconnect matrix: performance gate FAILED

FrameLink ac2100d plus renderer6e2ba57, same45s/100ms/ring8/queue6:

- NV1230:1350/1350 complete, all phases lossless.
- BGRA30:1349/1350 complete; one steady IPCpending replacement at9.600s.
  Preceding producer flSubmit took66.17ms, mostly before flEndSubmit, while
  renderer maintained~8ms iterations; subsequent catch-up arrivals~6.7ms apart
  exceeded the single latest-wins mailbox's polling cadence.
- BGRA60:2521/2700 complete,179explicit pre-submit ring_full failures.
- NV1260:2521/2700 complete,179explicit pre-submit ring_full failures.
- Both60cases have172steady ring_full failures. Every successfully submitted
  frame is acquired/adopted/rendered/GPU-completed/presented; no postsubmit loss.
- Independent known-source manifests and all terminal events reconcile every
  identity. Zero unknown loss does NOT mean zero dropped frames.

Preconnect removed the earlier late/catch-up startup defect, but changed timing
phase enough to expose remaining ring retirement/capacity pressure. Earlier
clean45s cases are retained, not substituted for this failing final matrix.
The ring budget remains nominal, not a demonstrated universal60fps guarantee.
Additional opt-in producer Signal/Flush and timeout-owner snapshots plus native
retirement observations are the next diagnostic step; no latency/ring size has
been changed to hide this failure.
