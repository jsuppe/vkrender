# Multi-display spanning (design)

Render one scene across N displays on Windows, Linux and Android, with
presentations that can span the wall or animate across it as they arrive.

Status: **design**. Nothing here is built yet except the canvas model, which
has existed since M15 and is the reason the rest is tractable.

## What already exists

`SceneHost` (desktop) is already a shared-render-context spanning model:

- A window is `WxH+X+Y`, and that rect is **both** its desktop position **and**
  its region of a **virtual canvas** ([scene_host.h](../renderer/scene_host.h)).
- The canvas is one world space: half-height `3.11`, half-width
  `3.11 * canvasAspect`, where the aspect is the union of all window rects.
- Each window renders that canvas through an off-center sub-frustum,
  `matFrustumSub(canvasAspect_, u0, v0, u1, v1)`.
- One `VkDevice`, one command buffer, **one submit**, one `vkQueuePresentKHR`
  across every swapchain.

```
        virtual canvas  (one world space, one scene graph)
   ┌──────────────┬──────────────┬──────────────┐
   │  sub-frustum │  sub-frustum │  sub-frustum │
   └──────┬───────┴──────┬───────┴──────┬───────┘
      swapchain 0    swapchain 1    swapchain 2
          └──────── one vkQueuePresentKHR ───────┘
```

**The consequence that makes the animation work cheap:** windows are viewports
into one continuous space, so a stream animating from `cx = +W` to `cx = 0`
slides across the whole wall with no cross-display coordination at all. There
is no "which display am I on" logic to write, and there never should be.

### Verified (M29)

Three windows tiled across one 5120x1440 monitor:

```
vkrenderer --serve vkrender --window 1100x900+10+30 \
    --window 1100x900+1120+30 --window 1100x900+2230+30
```

Result: `canvas 3320x900 (aspect 3.69)`, three surfaces, three swapchains,
**120 fps** with `present=8.1 ms` — one 119 Hz refresh, i.e. the extra two
swapchains cost nothing measurable. Per-window snapshots stitched side by side
show the vaporwave horizon, sunset gradient and perspective grid continuing
across both seams with no discontinuity.

The premise holds. The spanning model needed no changes to run at three.

## Gaps

1. **Android duplicates the scene.** `scene_host.cpp` is `#ifndef __ANDROID__`;
   `android_main.cpp` has its own single-window loop with its own `faceSlots`,
   `VideoPass` and backdrop handling. Any canvas or animation work done now
   gets written twice and drifts.
2. **No animation layer.** Layouts recompute static regions from the stream
   count every frame. A joining presenter pops into place; there is no
   *current* transform distinct from a *target*.
3. **One present strategy.** Lockstep across swapchains is correct only when
   the displays match and share a GPU.

## Architecture

### 1. `DisplayTarget`

Replace the desktop-only `Window` struct with a platform-neutral target:

```cpp
struct DisplayTarget {
    float u0, v0, u1, v1;      // region of the virtual canvas
    VkSurfaceKHR  surface;     // GLFW / Wayland / ANativeWindow
    VkSwapchainKHR swapchain;
    uint32_t refreshMilliHz;   // drives the pacing decision
    uint32_t deviceIndex;      // 0 today; non-zero = second GPU
};
```

The scene never asks what platform or display it is on — only for the target
list and their canvas regions.

### 2. Two present strategies, chosen by configuration

**Lockstep** (today). One submit, one multi-swapchain present, sub-frustum per
target. Zero extra bandwidth, native resolution per display, no resampling.
Correct when: one GPU, matching refresh.

**Canvas-blit.** Render the whole canvas once into an offscreen image, then
each display's present is a **blit of its sub-rect**. Correct when: mixed
refresh rates, or multiple GPUs.

The key detail that makes canvas-blit cheap: size the canvas image at the
**exact pixel union** of the displays, so every blit is a 1:1 copy — no
scaling, no quality loss. Costs one full-canvas write + read per frame and the
VRAM (triple 4K ≈ 100 MB). Trivial on a discrete GPU; genuinely expensive on a
mobile tile GPU, which is why this must not become the only path.

It also composes with work already done: M24's consumer path already blits the
scene into consumer images (`Extras::blits`), so the virtual camera becomes
another consumer of the same canvas image rather than a special case.

| | Lockstep | Canvas-blit |
|---|---|---|
| Mixed refresh | pacing collapses to slowest | each display paces itself |
| Multi-GPU | no | yes (one cross-device copy per GPU) |
| Bandwidth | minimal | +1 canvas write/read per frame |
| Mobile GPUs | good | poor |
| Resampling | none | none (1:1 at exact union size) |

Default to lockstep; switch to canvas-blit when the target set reports
mismatched refresh or more than one `deviceIndex`.

### 3. Unified `SceneHost`

Drop `#ifndef __ANDROID__` and make `SceneHost` the scene core on all three
platforms, with a thin per-platform surface provider:

| Platform | Provider |
|---|---|
| Windows | GLFW window → `VkSurfaceKHR` (exists) |
| Linux | GLFW/Wayland (exists); `VK_KHR_display` later for a true kiosk |
| Android | `Presentation` per display → `Surface` → `ANativeWindow` → `vkCreateAndroidSurfaceKHR` |

`android_main.cpp` becomes a thin shell like `win_service.cpp` already is.
This is the single highest-value change here, and it is worth doing **before**
the animation layer specifically so the animation is written once.

### 4. Android multi-display

Java side:

- `DisplayManager.getDisplays()`; a `Presentation` per non-default display,
  each hosting a `SurfaceView`.
- `surfaceCreated` hands the `Surface` down through JNI, exactly as
  `renderer_jni.cpp` already does for producer surfaces.
- `DisplayManager.DisplayListener` for hotplug → add/remove `DisplayTarget`s at
  runtime. The renderer already tolerates surface loss
  (`recreateWindow`/`VK_ERROR_OUT_OF_DATE_KHR`), so this extends existing
  machinery rather than inventing new.

#### Zero-copy survives multi-display for free

A reasonable worry is that surfaces must become display-specific and that this
would break the zero-copy import. It does not, and the reason is worth stating
because it constrains everything else.

The AHB import in
[external_video_image.cpp](../renderer/external_video_image.cpp) is bound to
`vk.device` and nothing else — `vkGetAndroidHardwareBufferPropertiesANDROID`,
`vkCreateSamplerYcbcrConversion`, then a `VkImage`/`VkDeviceMemory` on that
device. No surface, no swapchain, no display appears anywhere. An imported
producer buffer is a **sampled texture on the device**.

So with one `VkDevice` and N display swapchains:

- each producer buffer is imported **once** and sampled by **all** displays'
  render passes;
- a stream spanning two displays is the same `VkImage` sampled twice in the
  same command buffer — no copy, no duplication.

Display-specific state is confined to the **swapchain** (one `Surface` per
`Presentation`), which is our render target, never the producer's buffer.

This would only change if a producer buffer were **scanned out directly** by
the hardware composer, bypassing our render pass — then it would have to be
allocated compatible with that display controller and cross-display sharing
could fail. We composite everything, so producer buffers only ever need
`GPU_SAMPLED_IMAGE`, which is display-agnostic.

**The real Android multi-display risk is pacing, not buffers.** Single-submit
lockstep presents all swapchains together, so mismatched per-display refresh
(or independent SurfaceFlinger latching) stalls to the slowest. The canvas-blit
fallback that fixes this on desktop costs a full-canvas write and read — the
worst thing to ask of a tile GPU. Mixed-refresh Android multi-display is
therefore the hardest configuration in this whole design, and needs measuring
on the actual device before committing to a strategy.

#### Target hardware: QCS8250

The Android wall is a QCS8250 (Snapdragon 865 class, Adreno 650) driving
displays over **2x MIPI-DSI** and **DisplayPort 1.4 over USB-C**. Two things
follow, and they resolve the "different devices drive the displays" question:

- **One GPU, therefore one Vulkan physical device.** Those are different
  display *interfaces* on the same display subsystem, not different GPUs. The
  zero-copy analysis above holds unchanged: one import per producer buffer,
  sampled by all three displays' passes.
- **The USB display is DisplayPort alt-mode, i.e. real scanout** — not a
  DisplayLink-style USB graphics adapter that encodes and ships frames. Worth
  confirming on the device, because the two are indistinguishable from the app
  and only one of them is zero-copy.

#### Frame-accurate sync is the real constraint, and it is not free

The requirement is that a tile sliding across the wall must not tear at a seam.
Be clear about what Android can and cannot give:

- **Achievable:** the same logical frame on every display.
- **Not achievable:** the same *instant*. DSI and DP are independently clocked,
  Android has no cross-display genlock, and SurfaceFlinger's DispSync is a
  per-display software PLL locked to that display's own hardware vsync.

The best tool available is `VK_GOOGLE_display_timing`: a single
`vkQueuePresentKHR` carries `VkPresentTimesInfoGOOGLE` with a per-swapchain
desired present time, so all three can target one timestamp. That minimises
skew but does not remove it — each display still lands on its own next vsync,
so worst case is one refresh period, **16.6 ms at 60 Hz**.

That is the number that matters for the animation. A tile crossing the wall at
~1000 px/s is offset by ~16 px at a seam under one frame of skew. Whether that
reads as "torn" is a product judgement, and it is **the single biggest risk to
the animate-across-displays premise** — worth measuring before the animation
layer is built, not after.

One mitigation worth raising at bring-up: the QCS8250 has *two* DSI
interfaces. If two panels are driven as a single logical display in dual-DSI
split mode, those two are genlocked by construction and only the DP panel can
drift. That is a hardware/bring-up decision, not something the app can choose,
but it converts the problem from three drifting displays to two.

#### Skew compensation: two offsets, and a loop that measures itself

Skew does not have to be eliminated - it has to be *compensated*, and it can be
calibrated per display without designing around a number nobody has yet.

Two per-display values, both defaulting to zero:

| | Fixes | Mechanism |
|---|---|---|
| `presentOffsetNs` | whole-frame skew | which vsync this display targets, via the per-swapchain time in `VkPresentTimesInfoGOOGLE` |
| `timeOffsetNs` | sub-frame phase | the animation clock used for **this display's pass** |

The second is the one that actually saves the animation, and it is worth being
precise about why. A *presentation* offset cannot fix sub-frame phase: a panel
only scans out on its own vsync, so you may choose which vsync to aim at but
never a fraction of one. Instead, fix the CONTENT rather than the timing - if
display B lights up 8 ms after display A, render B's pass with `t + 8 ms`. When
B illuminates it shows the position the tile has actually reached by then, so
the tile lines up across the seam even though the panels lit at different
instants.

This is nearly free here: each display already renders through its own
sub-frustum in the same command buffer, so only the model matrices need
recomputing per display - a handful of matrices per frame.

**The loop measures itself.** `VK_GOOGLE_display_timing` also gives
`vkGetPastPresentationTimingGOOGLE`, which reports the ACTUAL present time per
swapchain. Comparing those across displays for one logical frame yields the
measured skew, from the device, with no camera or external instrumentation.
That matters because a static calibration decays - DSI and DP have separate
PLLs, so any rate difference makes the phase drift and a number measured once
goes stale. A feedback loop tracks it.

Consequence for sequencing: the compensation path can be **designed and built
now** with both offsets at zero, because the structure is identical whether the
real skew turns out to be 0 or 16 ms. Hardware is then needed only to confirm
the loop converges - not to make design decisions. This is what demotes the
"measure skew first" milestone from a blocker to a validation step.

Caveats to verify on real hardware, not assume:

- OEM support for `Presentation` varies enormously.
- Per-display refresh and density differ; `Display.getMode()` feeds
  `refreshMilliHz`.
- A tile GPU makes canvas-blit expensive — Android should stay on lockstep
  unless measured otherwise.

### 5. Animation layer

Per stream, a transform with a target, not a recomputed position:

```cpp
struct StreamTransform { float cx, cy, hw, hh; };

struct StreamMotion {
    StreamTransform current, velocity, target;
    enum Phase { Entering, Settled, Leaving } phase;
};
```

- **Layouts compute targets only.** They never write `current`.
- **Entry:** spawn off-canvas right (`cx = W + hw`), target = layout slot.
- **Reflow:** a joining or leaving stream changes every target; all streams
  glide from wherever they are.
- **Exit:** target off-canvas; free the slot when it arrives.
- **Span:** a target whose `hw` exceeds one display's share. No special case —
  it is just a wide target on the canvas.

**Use a critically-damped spring, not a fixed-duration tween.** The common case
is a second presenter joining while the first is still flying in, which
re-targets mid-flight. A spring absorbs that continuously; a tween has to
restart and visibly snaps.

Chosen motion (M28 discussion): slide in across the wall, existing tiles glide
aside, new tile eases into place.

### Built (M31)

`renderer/scene_motion.h` — header-only, no Vulkan or platform types, so the
desktop scene host and the Android wall demo share it and the motion exists
once. Semi-implicit critically damped spring per component, clamped `dt` so a
stalled frame cannot launch tiles across the wall. `VKR_MOTION_OMEGA` tunes
stiffness; `VKR_MOTION_OFF=1` snaps instead, which is the honest fallback if
cross-display skew ever makes motion across a seam unacceptable.

Verified on the Android wall (emulator, three panels, one submit):

- **entry** — tile appears beyond the right edge and travels across the wall to
  its slot;
- **reflow** — a second join re-targets both, and the survivors glide;
- **exit** — the leaver slides off the right and is reaped only once fully
  clear, confirmed by `tiles drawn` exceeding the presenter count mid-flight.

Verified on Windows (three windows, real WHIP presenter): the tile is caught
mid-flight crossing panels rather than at its settled position.

**Desktop cannot animate exits yet.** A departing stream's producer slot is
freed the instant it disconnects, so there is nothing left to sample and the
tile is skipped rather than drawn mid-exit; the survivors' reflow still plays.
The Android harness can do it because its tiles are procedural. Animating
desktop exits needs the service to hold a slot until `SceneMotion::leavingAt()`
clears - `leavingAt` exists for exactly that, it is simply not wired into
`win_service.cpp` yet.

## Constraints and open decisions

**Refresh mismatch is the sharpest edge.** FIFO across swapchains paces to the
slowest display. M28 measured how brutally presentation dominates this renderer
— a sleeping display took the whole scene from 120 fps to 7. A 60 Hz panel
beside a 119 Hz one will halve the wall unless canvas-blit decouples them.
This is the main reason the strategy split exists.

**Bezel compensation — built (M29).**

```
vkrenderer --serve vkrender --window 1100x900+10+30 --window ... --bezel 30
```

Model: a `--window` rect is that panel's **physical footprint** in canvas
space, so on a real wall the config gives each screen's position relative to
the others and *the gaps between those rects are the bezels*. `:bezel` (or the
uniform `--bezel`) then says how much of a panel's own edge its frame hides.

Implementation is two things:

- `SceneHost::init` builds each panel's sub-frustum from its rect **inset by
  the bezel**, while the canvas still spans the full physical footprints. So
  content landing under a frame is never drawn.
- `VideoPass::recordScene` renders into a viewport inset by the same amount;
  the cleared border left around it is the frame. `VKR_BEZEL_COLOR=r,g,b`
  makes it visible while tuning.

On a single monitor this doubles as a **bezel simulator**, which is how it was
validated: with `--bezel 30` and a red frame colour, the horizon sits at the
same height across all three panels and the perspective grid lines continue on
their correct trajectory through each frame. That is the test that matters —
content must be *occluded* by the bezel, not squeezed inside it. Without
compensation, insetting compresses each panel and every grid line kinks at the
seam.

**One GPU is assumed initially.** `deviceIndex` exists in the struct so
multi-GPU is not designed out, but the first implementation should reject a
mixed-device target set with a clear error rather than silently misbehave.

**Layouts do not adapt to a wide canvas.** Found during M29: with two streams
on a 3.69-aspect wall, the Presentation layout stacks them top/bottom in the
*middle* and leaves both ends empty. Its regions were designed for a 16:9
canvas, and `sx = min(hw, hh * 16/9)` aspect-fits each tile so wide walls go
unused. A three-panel wall wants two streams side by side, not stacked. The
layout region tables need to key off `canvasAspect` — worth doing with the
animation work, since both touch the same target computation.

**Open:** which Android device provides the second/third display, and what its
`Presentation` support and per-display refresh actually are. That determines
whether Android gets lockstep only or needs the canvas path too.

## Suggested milestones

| | Work | Value |
|---|---|---|
| ~~M29~~ | ~~Verify 3-window spanning + bezel compensation on Windows~~ | **done** — spanning seamless at 120 fps, bezels occlude correctly |
| M30 | Unify `SceneHost` across all three platforms | Stops the Android fork; everything after is written once |
| M31 | `DisplayTarget` + present strategies (lockstep / canvas-blit) | Handles mixed refresh and future multi-GPU |
| ~~M31~~ | ~~Animation layer (spring transforms, entry/exit/reflow)~~ | **done** — `renderer/scene_motion.h`, shared by desktop and the Android wall |
| M32.5 | Per-display `presentOffsetNs` / `timeOffsetNs` + the self-measuring skew loop | Buildable now with both at zero; hardware only confirms convergence |
| M33 | Android multi-display via `Presentation` + hotplug + `VK_GOOGLE_display_timing` | Third platform |
| M34 | Bezel compensation + 3-display validation on all platforms | Finish |

M29 first is deliberate: it is an afternoon, and it either validates the whole
premise or surfaces the problem before five milestones are built on top of it.
