# Native tracing off/on overhead and qualification gaps

Same FrameLink cbe7352 / renderer cc527ba binaries as native-capacity-20261031, native RTX5090 window,1080p60,12 slots,six pending,100ms. Four45-second cases: BGRA off/on then NV12 off/on. Tracing-on includes detailed renderer/transport/GStreamer events, retirement GPU-counter probes and ring timeout snapshots; capacities262144 events/thread. No Khronos layer. Tracing-off removes all five enable variables; noCSV files were generated. Existing original demo unaffected.

## CPU measurement

Windows GetProcessTimes kernel+user CPU sampled through each live process handle alongside the identical50ms status requests. Primary window is wall5–40s; percentages are **one logical CPU core**, not total-machine utilization. This is one matched sequential pair per format, not a statistically repeated overhead benchmark; scheduler/load/phase differences remain. Trace-file shutdown flush costs are outside this active-playback window.

- BGRA renderer:2.06%off →5.90%on (+3.84 percentage points). Producer42.63%off →41.07%on; the decrease is variation, not a claim tracing makes production cheaper.
- NV12 renderer:23.29%off →28.71%on (+5.42 points). Producer45.96%off →47.18%on.
- Secondary post10s–40s CPU/rate summaries are retained to distinguish the early BGRA transient; they do not erase its failure. All four secondary sampled due-decision rates were approximately60/s.

Enabled exhaustive tracing demonstrably has nonzero CPU overhead. These measurements do not isolate recorder cost from optional GPU-counter queries, and do not establish disabled-path overhead versus a compile-time-stripped build.

## Outcomes — not a universal qualification pass

- BGRA off:2700 received,2691 submitted,9 ring-full drops. Status late counter reached31; fresh-adoption five-second windows217,266,then300s. Sampled5–40s due rate59.03/s. Late jumps occurred near wall1.47–2.08s and6.43–6.58s, with schedule error reaching380ms. Exact missing identities/driver cause unavailable with tracing disabled.
- BGRA on:2700/2700 accepted unique presentations, exact expected-source accounting complete, zero startup/steady/end loss.
- NV12 off:2700/2700 producer submissions, no reported ring/late/import failures, observed due counter2700, approximately60/s. Aggregate evidence only, not per-frame GPU/presentation identity proof.
- NV12 on:2697/2700 accepted identities. Exactly three startup IPC latest-wins replacements atPTS100000000,166666666,216666666ns. All2580 steady and60 end identities complete. Maximum startup unique-presentation gap46.57ms; steady maximum16.90ms. Exact source universe and all terminal outcomes closed.

No additional captures or tuning were performed to hide the failures. These results expose early transient loss not seen in the preceding clean matrix; they block an unconditional lossless qualification. They do not show that tracing causes or prevents the transients. Complete accounting is not lossless delivery. Presentation means API acceptance; physical scanout is unmeasured. Repeated scene120Hz submissions are not fresh videoFPS. Native playback-complete's video_received=0 belongs to the unused GStreamer video branch; use native due/adoption counters instead.

## Longer steady gate remains unexecuted

Current producer/consumer CLI validates timeout-ms<=60000; renderer uses that sample startup/runtime budget. The probe accepts1..50 seconds and adds8 seconds setup/drain allowance. Consequently a>=60-second steady interval plus startup/drain cannot be qualified within the current watchdog contract. Extending only the harness would exceed the supported sample timeout; no limits were silently enlarged. A separately reviewed long-running test/process-budget design is required before claiming that duration.
