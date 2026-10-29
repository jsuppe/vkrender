# Android Binder moderation control

The signature-protected `dev.vkrender.IVkRendererService` now appends four methods
without changing existing Surface acquisition or ownership transaction IDs:

- `getControlVersion()` returns 1.
- `getControlCapabilities()` returns JSON. Moderation is available; camera gate,
  preview transfer and display routing are explicitly unsupported by this API.
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
qualifies Binder controls; FrameLink kick and the unavailable camera/preview/
display operations are not relabeled as device-qualified features.
