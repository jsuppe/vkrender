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

Three displays should work today on Windows/Linux with:

```
vkrenderer --serve vkrender --window 1920x1080+0+0 \
    --window 1920x1080+1920+0 --window 1920x1080+3840+0
```

Untested at three. That is the first thing to verify, before any new code.

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

**Bezel compensation.** Physical bezels mean the canvas should optionally be
*larger* than the sum of the visible pixels, with each display's `u0..u1`
insetting to account for the gap — so a sliding tile passes *behind* the bezel
instead of jumping. Cheap to add to the rect maths; needs a per-display bezel
value in config.

**One GPU is assumed initially.** `deviceIndex` exists in the struct so
multi-GPU is not designed out, but the first implementation should reject a
mixed-device target set with a clear error rather than silently misbehave.

**Open:** which Android device provides the second/third display, and what its
`Presentation` support and per-display refresh actually are. That determines
whether Android gets lockstep only or needs the canvas path too.

## Suggested milestones

| | Work | Value |
|---|---|---|
| M29 | Verify 3-display spanning on Windows today; fix what breaks | Proves the existing model before building on it |
| M30 | Unify `SceneHost` across all three platforms | Stops the Android fork; everything after is written once |
| M31 | `DisplayTarget` + present strategies (lockstep / canvas-blit) | Handles mixed refresh and future multi-GPU |
| M32 | Animation layer (spring transforms, entry/exit/reflow/span) | The visible feature |
| M33 | Android multi-display via `Presentation` + hotplug | Third platform |
| M34 | Bezel compensation + 3-display validation on all platforms | Finish |

M29 first is deliberate: it is an afternoon, and it either validates the whole
premise or surfaces the problem before five milestones are built on top of it.
