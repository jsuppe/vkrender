# M5 final integration — 2026-11-01

Renderer151ecd6 rebuilt successfully on Linux against FrameLinkec4d546, including
its new GStreamer audio dependency. Targeted av_schedule, av_clock_contract,
headless_pacer and nv12_cpu tests passed4/4. CI pins FrameLink21d15d5 (same runtime source plus the controlled-clock
regression correction) and includes these portable tests on Windows/Linux.

This does not repeat or broaden the accepted Windows sustained captures. See
[sustained parent review](sustained-qualification-20261031/PARENT_REVIEW.md):
BGRA/NV121080p60, four70s cases; each traced case4200/4200 identities through GPU
completion and API-accepted present,59.993fps steady; untraced aggregate checks.
Original failed controls and trace-overhead limits remain preserved.

FrameLink's signed Android cross-UID example and OpenSLES phase-alignment repair
are qualified separately, at160x90/30fps with CPU GStreamer/Surface rendering.
They do not establish native Android renderer A/V parity or physical lip-sync.
Explicit Android audio-master diagnostics remain unqualified. Native GStreamer
buffer allocation/zero-copy negotiation is M5a, not this merge.

No claim of physical scanout, acoustic synchronization, cold display wake,
seamless route/suspend handling, or zero transport latency is made. The final
capability matrix lives in FrameLink docs/m5/CAPABILITY_MATRIX.md.

## Hosted Windows clock-test deployment correction

Run41308311506 built all targets; Linux, Android and sender passed. Windows
passed10 portable cases and then remained at av_clock_contract for more than
three minutes; the parent canceled it instead of leaving an unbounded gate.
That run is not counted as a Windows pass. Its test-step output is preserved in
`windows-clock-ci-41308311506.log`.

The workflow installed GStreamer but did not expose its bin directory to later
processes. Fresh Caspar execution of the same existing clock-test binary without
the runtime path failed (exit-1073740612,25ms); with the configured runtime path
it passed in55ms. This establishes a runtime-loader dependency, not the exact
mechanism of the hosted runner's longer wait. The corrected workflow explicitly
exports the detected SDK/bin and adds per-test/step time bounds; av_clock_contract
also has a30s CTest limit. No renderer/runtime source is changed. A new hosted
run must pass before this PR can merge.

Corrected runtime environment: all13 portable cases passed on Caspar in3.14s;
Linux13/13 passed in1.94s after reconfiguration. Existing test binaries were used
on Caspar; only workflow environment/timeouts and CMake test metadata changed.
