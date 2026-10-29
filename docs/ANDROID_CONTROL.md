# Android Binder moderation control

The signature-protected `dev.vkrender.IVkRendererService` now appends control and preview methods
without changing existing Surface acquisition or ownership transaction IDs:

- `getControlVersion()` returns 1.
- `getControlCapabilities()` returns JSON. Moderation and secondary display routing are available; camera gate is
  unsupported. Preview capability is
  negotiated from the active Vulkan format.
- `getControlStatus()` returns last published JSON: `ready`, `pending`, `applied`
  and `state` (layout, pause, config, stream roster and visibility).
- `submitControl(action, slot, value)` queues a command. It never waits for Vulkan
  or a render-thread acknowledgment.

Return codes: **0 queued**, **1 invalid**, **2 unsupported**, **3 unavailable**,
**4 queue full**. Queued does not mean presented: observe the subsequent status.
`applied` counts processed commands. Commands pending when the Activity pauses,
loses its Surface or is destroyed are cancelled; `ready=false` and new commands
return unavailable immediately. Status remains readable while inactive.

Global actions: `layout` with `presentation`, `two-cubes`, `single-cube`, `wall`;
`pause` with `0`/`1`; `max_visible` with 1..24; `rotate_seconds` with a finite
number 1..86400. These use slot -1. Stream actions use an empty value and the
current **control roster** slot: `hide`, `show`, `pin`, `unpin`, `spotlight`,
`unspotlight`, `front`, `kick`. Unspotlight can use slot -1.

FrameLink inputs own control slots 0..11. Binder streams own 12..23; this is not
an app's input-Surface slot number. Binder control slots remain stable during a
live reader generation and may be reused after disconnect. Refresh the roster
before acting. Every signed controller can moderate the full scene; this does
not bypass the existing signature permission or allow unsigned applications.

Commands run on the render thread. All inputs continue acquiring and retiring
buffers while hidden or rotated out, preventing producer backpressure deadlock.
Binder kick uses existing generation-aware departure retirement; FrameLink kick
quiesces rare in-flight work before closing its ring/imports. Keyboard stream
selection retains its original registry-order behavior.

## Validation

`android_control_mailbox` is a CPU test of validation, capability rejection,
readiness, queue bounds and cancellation, with no Android/GPU claims. APK builds
compile native Android hooks, JNI and appended AIDL methods.

Debug APK only, `ControlTestService` runs a real Binder caller in a separate
process. Start it while the renderer is visible:

```
adb shell am startservice -n dev.vkrender.renderer/.ControlTestService
adb logcat -d -s RendererControlTest:I
```

It validates schema/capabilities, rejected inputs, queued actions, hidden producer
progress (120 frames), Activity pause/unavailable/resume and kick. It navigates
Home and returns to the renderer, then stops itself. This is not presentation
latency or physical multi-display qualification.

Recorded validation (2026-10-29): arm64-v8a and x86_64 debug APK builds passed.
Four CPU tests passed: mailbox, all five AIDL copies/legacy transaction order,
Binder scene generations and existing StreamOwners regression. On the physical
Pi, the separate-process Binder fixture passed, including 120 hidden-source
posts without stalling and bounded unavailable response during Activity pause.
Raw evidence: `android-control-20261029/pi-control.log`. The renderer was stopped
and the existing virtual-camera demo task restored afterward. This device test
qualifies Binder controls; FrameLink kick and the unavailable camera gate are not relabeled as
device-qualified features. Preview/display validation is recorded below.

## On-demand preview

`getPreviewJpeg()` requests a snapshot and immediately returns the latest JPEG
or null when unavailable/pending. It is not a blocking screenshot call. Maximum
size is 320x180 (aspect preserved), 128 KiB, and five captures per second.
Readback commands run before presentation in the existing frame command buffer;
results retire only after its existing fence. No extra GPU wait or readback occurs
when preview is unused. Activity pause invalidates pending/results; callers must
retry after resume. JPEG encoding runs on the Binder thread and is cached by
capture sequence. `previewCaptures` in status reports completed snapshots.

The CPU `preview_mailbox` test covers request/rate/size/epoch bounds. The debug
Binder fixture additionally decodes a real GPU snapshot, checks scene content,
verifies no idle readbacks and verifies paused snapshots are unavailable.

Physical Pi validation passed for this path: GPU-produced scene decoded from
JPEG (green center, luminance range 5..172), no idle captures, bounded pause and
resume. Evidence: `android-control-20261029/pi-preview.log` (includes failed
pre-fix runs) and `preview.jpg`. Direct scaled blitting from the presentable image
produced black on this device; a full-size GPU image copy followed by scaling
passed. This extra copy is performed only for an actual snapshot request. Two
full-size staging images are retained after first use, protected by frame fences.

## Secondary display control

`getDisplayOutputsStatus()` reports Activity readiness, actual display IDs and
eligibility, selected state, crop and Surface readiness. Its separate `native`
array reports created swapchain extents and actual submitted presentation counts
(updated at most every 60 render frames). Selected is not equivalent to presented.

`configureDisplayOutput(id, enabled, x, y, width, height)` queues a normalized crop
on an eligible non-primary presentation display. Invalid/nonexistent/primary IDs
and nonfinite/out-of-bounds crops return invalid (1); inactive Activity returns
unavailable (3). Maximum 128 pending UI commands, with queue-full (4). Accepted
(0) means queued, not applied. Stop/restart invalidates queued commands. UI owns
Presentations; acquired native windows move through the render-thread registry.
No readback is introduced by secondary outputs. Existing signature permission
also protects these methods. Camera remains explicitly unsupported.

Physical Pi combined Binder fixture passed with secondary display 4: enable,
observe native presentation, request preview while secondary output is active,
crop update, detach, invalid ID/crop and paused rejection. Initial test detected
that onStop was too late to reject controls during onPause; readiness now follows
onResume/onPause, and a lifecycle epoch rejects obsolete queued commands. Logs
retain that failure and final pass in `android-control-20261029/pi-display-control.log`.
The renderer was stopped and existing virtual-camera task restored after testing.
