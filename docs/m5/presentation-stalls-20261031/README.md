# Windows native graphics stalls — causal investigation

Single executor; same native preparation path, pool12/pending6/common100ms.
This is not physical scanout qualification. An accepted present call and a GPU
fence observation are different software stages.

## Retained failure: not just a present queue

Original delayed-preparation capture in `../native-preparation-20261031`:

- First present call: source-relative105.518–268.878ms. Producer D3D11
  `Signal(ready)` simultaneously occupied117.303–267.504ms. Source delivery
  paused158ms and then caught up; only one IPC identity arrived inside present.
- Second present call:727.119–822.938ms. Producer Signal occupied
  735.141–820.889ms; another33.032ms Signal delayed recovery afterward.
- All180 were submitted;159 completed,16 late and5 unread replacements.
  Do not attribute all21 losses to IPC arrivals during present.

A fresh same-binary180-frame native reproduction under scheduler ETW delivered
154/180:23 late+3 unread replacements, all startup; steady60/60 and end60/60.
Here the renderer's163.011ms stall was in **queue submit**, not present. A later
97.819ms stall was in CPU mapping. Producer Signal stalled154.506/88.660ms.
Thus changing FIFO alone is not supported by this evidence.

## Scheduler evidence

ETW PROC_THREAD+LOADER+CSWITCH+DISPATCHER with CSwitch/ReadyThread stacks.
No lost events or buffers. The targeted excerpts retain native ETW relative
microseconds; call alignment uses each CSV's original steady/wall clock pair
and ETW's2026-10-31T14:28:11.0763455Z start. No PTS restamping.

- Renderer main thread47808: Waiting/WrResource162.932ms, of which only0.802ms
  after ReadyThread. Stack includes Vulkan NVIDIA user driver and dxgkrnl.
- Producer driver worker53344: Waiting/WrResource153.200ms; same csrss thread
  readies this worker and renderer within7microseconds.
- Producer video thread38108: Waiting/UserRequest154.288ms, readied by that
  driver worker; stack is FrameLink→D3D11→NVIDIA driver wait.
- Later renderer Map wait:97.608ms off-CPU, only0.039ms runnable afterward.

These measured spans are predominantly **blocked graphics-stack waits**, not
runnable-thread starvation or measured GPU execution time. They do not expose
the exact internal NVIDIA/kernel resource or prove historical BGRA380ms cause.

Raw ETL/full dump stay private on Caspar because they include unrelated process
metadata. Only task-specific wait/stack excerpts are intended for publication.
An OBS Vulkan capture hook appeared on ordinary present stacks; mere presence
is not causal proof, and no user applications/layers were disabled.

## Resolved shared-resource cause

Exact Microsoft PDBs (GUID/age and download provenance retained) resolve the
csrss readier to:

`NtUserUserPowerCalloutWorker → PowerOnMonitor → DrvSetMonitorPowerState →
DrvSetWddmDeviceMonitorPowerState → DxgkPowerOnOffMonitor →
DXGADAPTER::ReleaseCoreSync / ReleaseCoreResource`.

This is actual monitor power-on sharing the adapter core resource, not a
sleep inference from old comments. It explains why a producer in another
process also stops in D3D11 and why the renderer's blocked call can vary.
The initial call site is not necessarily the complete cost: recovery includes
source catch-up, bounded mailbox replacement and explicit late-frame policy.
Later unrelated power/driver transitions remain possible.

## Controls and bounded correction

Warm original wake behavior and `VKR_NO_DISPLAY_WAKE=1` each delivered180/180.
Both lacked ETW, so do not infer either tracing or wake injection caused the
first failure. The cold/warm state is a missing control.

The read-only console-display observer stayed ON despite the configured300s
AC timeout. The requested natural-off control was not launched. A subsequent
measured idle-but-ON case (389.7s input idle, screen saver disabled) also delivered
180/180; its first power request began104.3195ms after source start. These later
warm observations do not negate the earlier ETW PowerOnMonitor stack. No global
settings were changed and no monitor-off command or unrelated process stop ran.

The source fix requests system/display power **during A/V session setup, before
onSession acknowledgment and the producer's PRIMED/GO barrier**, instead of the
first timed adoption. The hold lasts through the timed session, including late
frame drops; session end/failure/teardown releases it. Headless-only output still
holds SYSTEM only, and legacy unscheduled slots retain their active-frame policy.
Existing input-wake opt-out remains. No added sleep, queue, pool, playback
latency, frame discard, timestamp reanchor or fence-ownership change.

Primary swapchain logs now report actual selected mode, image count and window
state; `service.power_request`/`service.display_input` are optional recorder
scopes. No per-frame new timer/syscall is added when tracing is disabled.
`VKR_TEST_AV_LATE_POWER_REQUEST=1` restores old ordering only for a negative.

## Affected validation

Matching Windows Release binary: see fixed-gates hashes. Khronos layer is
explicitly enabled in all five native short gates and the headless gate, no VUID.

- Native preparation positive:180/180 all identity stages, startup/steady/end
  60/60 each. Power request completed758.7ms before first source.
- Old-order negative: all180 delivered in this warm run, but the new ordering
  guard correctly rejects the power request108.6ms **after** first source.
  This is an ordering negative, not a claimed reproduced hardware stall.
- Lazy250ms preparation negative:158/180;10 ring-full+5 unread replacements+
  7 late. Even across these drops, exactly one pre-source hold and one terminal
  release: no drop-induced wake cycling.
- Preparation failure and deadline: both peers[1,1], zero GO/source media,
  unchanged phase deadlines. No failure-to-success filtering.
- Headless positive:180/180 GPU-complete; SYSTEM hold only, no display input.
- Linux Release build and affected av_schedule/av_clock_contract/nv12_cpu3/3.
  Windows Release build passed.
- Normal45s BGRA60 and NV1260: each2700/2700 all identity stages including GPU
  completion and accepted present. Startup60/60, steady2580/2580, end60/60 for
  each. About60.0fps, maximum unique present-call gaps25.0106/25.0474ms. Power
  requested458.1/473.2ms before first source, one terminal release each.
- BGRA45s tracing OFF:2700 received/submitted/due, zero ring-full, late, sequence
  gaps or present errors; no CSV files created. Sampled schedule error maximum
  +8.3086ms. Startup minimum−91.7181ms is a future-frame hold, not lateness;
  after first due decision the range was−8.3309..+8.3086ms. Aggregate regression,
  not identity/GPU/scanout proof. The earlier failed trace-off window is retained.
- Final `powercfg /requests`: no DISPLAY or SYSTEM request leaked.

Sources: renderer78b13ec (base63052aa); FrameLink
2fe7987 equivalent already-built preparation path unchanged. Source pipeline
SHA256 for all short180-frame comparisons:
`61e8f3265ef032ddcce6d161f4c37873e35234e7f1a4c4910c0fceecf6669942`.
The final native swapchain was FIFO,3images,1280x720 windowed/visible/not iconified;
initial VkContext mailbox creation was subsequently replaced by SceneHost FIFO,
both explicitly logged. Native45s rate checks did not enable validation layers;
short ownership/error-path gates did. All capture revisions and negative outputs
are kept; no frame-window selection.

This correction removes the known **late power-request ordering** defect. It
is not a guarantee that Windows has physically finished an asynchronous monitor
wake before media, nor a post-fix cold-power-on qualification: no natural OFF
state occurred during the observer window. The old untraced BGRA380ms excursion
has no historical identity/power record, so shared cause remains unproven.
Physical scanout and sustained final M5 qualification remain separate gates.
