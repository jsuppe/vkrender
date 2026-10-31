# Final native sustained qualification — 2026-10-31

**PASS for the measured Windows native BGRA/NV12 1080p60 workload.** Four
sequential 70-second cases, each run once, with startup and drain retained.
Both traced cases contain **4,200/4,200 independent source identities at every
required stage, including observed GPU completion and accepted presentation**.
Both trace-disabled cases pass aggregate counters only. This is not universal
losslessness, physical scanout, cold-display-wake, or Android qualification.

Runtime revisions: FrameLink **69d7b07**, renderer **7f58e38**. The latter includes
parent-reviewed power ordering at115293d and earlier preparation/capacity fixes.
No pool, pending limit, latency, late threshold, power policy, original demo,
or global monitor setting changed for these captures.

## Bounded finite-run contract

See FrameLink [FINITE_MEDIA_BUDGET.md](https://github.com/jsuppe/framelink/blob/69d7b07/docs/FINITE_MEDIA_BUDGET.md).
The sample peers negotiate exact finite budgets in opt-in mode3, max300000ms;
old peers reject it. CLI/API defaults remain legacy. Setup remains15000ms here,
steady socket/progress idle remains1–2s, and the active deadline is media budget
plus the unchanged setup/drain grace. This is not live/infinite or M6 soak work.
Explicit overflow/malformed values, mismatch and legacy peers are rejected.

The 70s harness uses `--media-run-ms 70000` and `--av-media-run-ms 70000`, **not**
longer startup timeouts. Fixed source:4200 ball video frames +4200 silent PCM
blocks,800 samples/block at48kHz stereo. Native1280x720 window displays1080p60
input, final primaryFIFO/3images; pool12/pending6/common100ms. Windows/Caspar
RTX5090 environment is the same as the preceding power-ordering evidence.

## Full run results (no selected favorable window)

Startup is source PTS[0,1s), steady[1s,69s), drain[69s,70s). Every source identity
is included; expected IDs are constructed independently from4200 ordinals and
60fps, not inferred from observed rows. Only the observed channel namespace is
used in the expected manifest. Complete per-stage/phase ledgers are retained.

- **BGRA/on:** startup60, steady4080, drain60 complete through every stage.
  Steady source67.982155s /60.001040fps; accepted unique present67.991199s /
  **59.993059fps**. Max unique present gap25.0115ms over the whole run.
- **NV12/on:** startup60, steady4080, drain60 complete through every stage.
  Steady accepted unique present67.991265s /**59.993001fps**; max whole-run
  unique present gap25.0830ms (steady25.0331ms).
- **BGRA/off:**4200 received/submitted/due, zero ring-full/late/sequence gaps or
  present errors; no CSV emitted. Measured aggregate activity minus fixed first
  and last1s =**67.961025s**. No per-identity or GPU completion proof in this case.
- **NV12/off:** same clean4200 aggregate counts; measured trimmed aggregate
  interval**67.950518s**. No CSV; same identity-proof limitation.
- All eight application exits were0. No case was repeated until passing.
  All sampled queues stayed within pending6/audio4. No present errors.

Source-to-accepted-present observations (not physical display latency):

- BGRA median111.7138ms, p99116.0775ms, max116.5862ms.
- NV12 median111.7405ms, p99116.1865ms, max116.5836ms.

Observed source-to-GPU-completion max153.5061ms BGRA /151.2622ms NV12. Whole-run
GPU-completion observation gaps max44.9115ms /40.0289ms occur in drain; steady
max25.0816ms /27.5390ms. These are host fence observations, **not GPU execution
durations**. All final identities completed; nothing was omitted from drain.
Present-call CPU max22.8797ms /23.0430ms, submit max0.1544ms /0.1520ms;
NV12 CPU-map max3.4205ms. No recurrence of the measured~163ms adapter stall.

## A/V and PCM consistency

Each of the four cases completed4200 PCM blocks/**3,360,000 samples/channel**,
with first audio running PTS0, audio and video ends70,000,000,000ns, common100ms
latency, zero late PCM blocks, and every PCM block admitted through the existing
sample-offset/duration continuity validator. Final PCM arrival-drift diagnostics:
BGRA/on+0.375633ms, BGRA/off+0.011433ms; NV12 values are in `metrics.json`.
The native video's generic playback log first-PTS field is an unavailable
sentinel because native video bypasses that appsink; traced video identity0
is separately verified. No timestamp rewriting or per-hop reservoir was added.

This is timeline/sample-count continuity, not a full-run PCM payload hash,
acoustic A/V alignment, or a physical device-underrun measurement. Queue-full
and queue-empty events are bounded pipeline telemetry, not device underruns.
Sampled off schedule error max+8.370167ms BGRA/+8.331617ms NV12; negative startup
values are future holds, not late output.

## Tracing capacity and measured overhead

Fixed capacity**524288 events/thread**. Largest actual thread216022 BGRA /
232864 NV12. Every footer reports dropped_events=0 and rejected_threads=0;
no trace truncation/loss. Trace-on includes ring and GPU retirement/query
diagnostics. Trace-off removes all enable flags and emits no CSV.

Steady CPU percentages relative to one logical core, on/off:

- BGRA renderer4.5763/4.1639%, difference**+0.4124 percentage points**;
  producer40.6352/38.9018%, difference+1.7333pp.
- NV12 renderer28.3171/28.2320%, difference**+0.0852pp**;
  producer46.0527/47.7666%, difference−1.7139pp.

One sequential pair per format, not randomized or a causal overhead benchmark.
The negative producer difference illustrates run-to-run variation; it is not a
tracing benefit. No zero-penalty claim. CPU sampling excludes shutdown CSV flush
and includes diagnostic queries. Earlier measured+3.84/+5.42pp pairs remain
valid evidence of their runs, not replaced by these smaller differences.

## Affected validation and preserved failures

- Linux FrameLink and renderer Release builds passed. FrameLink offer bounds,
  audio segment, recovery3/3; renderer schedule, clock contract, NV12 CPU3/3.
- Linux actual finite peers:8 cases pass, plus explicit source-stall case exits
  in2.571s despite70s budget. Strict8 malformed CLI budgets rejected; unit wire
  bounds include uint64 max and overflow-safe duration checks.
- Windows matching Release builds;9 finite real-peer cases +8 malformed CLI
  cases + bounds unit passed.4s playback succeeds under2s setup limit;
  missing peer2.077s, source stall2.645s, earlyEOS0.864s, API cancel0.661s.
  Silent stopped peers fail boundedly; fixture process handles only are stopped.
- Windows native Khronos gates with70s budget: preparation180/180 and early power
  ordering pass; failure and original4s preparation timeout yield[1,1], noGO.
  No VUID diagnostic in retained logs. Default legacy recovery tests also pass.
- Preserved **initial Linux harness failure** in `linux-regressions/m5-finite-linux-1`:
  suspended-consumer test expected producer exit within4s. The OS accepted all
  four seconds of PCM, then the unchanged2s DONE socket timeout applies. The
  test was corrected to a7s wait (same media/source/timeouts); measured6.134s.
  No heartbeat or immediate detection of kernel-buffered delivery is claimed.
- Windows combined build compiled renderer/peers, then the requested test target
  was absent because renderer CMake forces FrameLink testsOFF. Separate matching
  `build-finite` was used for regression executables; no runtime source workaround.
  A first standalone configure also warned about a split, unused pkg-config
  argument; quoted reconfiguration succeeded. These setup failures are not
  suppressed performance failures. Historical native failures remain unchanged.

## Reproduction and artifacts

The maintained probe is FrameLink `test/m5_renderer_rate_probe.py` at69d7b07.
Exact commands, outputs, process codes and original CSV timestamps are in each
case folder (JSON/CSV/manifest files gzip-compressed). The four PowerShell run
scripts record the interactive scheduled-task launch environment. They run one
case at a time; `--trace on|off --native --seconds 70 --clock-policy producer`.
`regression.ps1` and `preparation.ps1` record short gates.

`analyze.py <decompressed-case-root> --accounting-tool <FrameLink>/tools/frame_accounting.py`
checks all counters, bounded queues, trace footers, independent source universe,
all phases and actual steady spans/rates. `metrics.json` is the compact result;
full per-frame accounting remains alongside raw traces. `export_evidence.py`
allowlists only logs/CSV/JSON/source pipelines and excludes rendezvous/session
credential directories. No ETW or unrelated host-process capture was recopied.

Final lane state: no task renderer/producer/compiler process left; interactive
helper Ready; DISPLAY/SYSTEM power requests None. Original demo untouched.
Cold physical power-on after the fix remains unqualified (no observed OFF),
as do physical scanout/acoustic sync and the historical untraced BGRA380ms cause.
M5 as a whole remains open for Android adoption/cross-UID and final review.
