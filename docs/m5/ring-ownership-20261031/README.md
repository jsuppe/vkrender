# Native Windows ring ownership probes

Same 100 ms latency, eight transport slots, six pending video samples. Opt-in Signal/Flush and retirement/ring-state tracing only; no capacity or pacing changes. Source: renderer d093a3e (includes per-image presentation semaphores), FrameLink preconnect ac2100d plus Windows transport scopes from b9c7602. Release build, RTX 5090, interactive limited desktop. This ownership probe did not enable Khronos validation; separate semaphore/lifetime validation evidence is retained under native-accounted-20261031. Raw traces compressed losslessly. Manifests independently enumerate fixture FPS × duration, not observed sink rows.

## Outcomes

- BGRA 60, 10 seconds: 600 source identities, 560 accepted unique presentations, 40 explicit ring-full drops.
- NV12 60, 10 seconds: 600 source identities, 598 accepted unique presentations, two startup drops. This phase did not reproduce sustained pressure; retain this result alongside the failing combined 45-second matrix.
- BGRA 30, 45 seconds: all 1350 identities accepted. The earlier 66 ms producer submission stall was **not reproduced**. Signal maximum 1.362 ms, Flush maximum 0.013 ms, submission maximum 1.377 ms. These are call wall times, not driver execution measurements. The earlier stall remains unresolved, not assigned speculatively to Flush.

All three independent-manifest analyses close every expected identity and repeated draw tuple; complete accounting is not lossless delivery. GPU completion is fence observation, presentation is API acceptance, neither is physical scanout.

## Decisive ownership snapshot

BGRA60 channel 1638118284386676518, timeout at steady_ns 1404341661214700: producer highest ready 119, consumed 111, all eight slots occupied by ready values 112–119. Renderer current=113, pending depth=6 (114–119), prior GPU read=112. Thus six future + current + one earlier GPU owner exhaust the ring; no producer-write slot remains.

Retirement target 112 was submitted 16.537 ms before timeout. Observed consumed remained 111 at -8.189 ms and at timeout; it reached 112 at +0.306 ms. Across 40 timeouts, 37 had pending=6 and current−consumed=2; three had pending=5 and current−consumed=3. Therefore at least nine slots would be necessary for these particular ownership snapshots plus a writer, but this **does not prove nine sufficient** under other mailbox/in-flight/jitter phases.

The capacity contract must count unique owners across future scheduled samples, current sample, the single latest-wins IPC mailbox, outstanding GPU reads and producer writes without double-counting a buffer shared by multiple draws. SceneHost has two frames in flight. Both FrameLink's kMaxPool=8 and renderer legacy kMaxPoolSize=8 size fixed wire arrays; increasing constants is a protocol/compatibility change, not an arbitrary local tuning knob. No capacity change was made here.
