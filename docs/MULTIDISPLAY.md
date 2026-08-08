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
| M32 | Animation layer (spring transforms, entry/exit/reflow/span) | The visible feature |
| M33 | Android multi-display via `Presentation` + hotplug | Third platform |
| M34 | Bezel compensation + 3-display validation on all platforms | Finish |

M29 first is deliberate: it is an afternoon, and it either validates the whole
premise or surfaces the problem before five milestones are built on top of it.
