# Android secondary display output

Opt-in secondary outputs use Android `DisplayManager` / `Presentation` and
`SurfaceView`. The primary Activity/renderer remains the scene clock and output;
with no `displayOutputs` option its behavior remains unchanged.

Launch or update a foreground renderer (the single-top flag delivers new extras):

```sh
adb shell am start -n dev.vkrender.renderer/.MainActivity -f 0x20000000 \
  --es displayOutputs all
# Explicit display IDs, center-half crop on display 4:
adb shell am start -n dev.vkrender.renderer/.MainActivity -f 0x20000000 \
  --es displayOutputs 4 --es displayCrops 4=0.25,0.25,0.5,0.5
# Stop secondary outputs without stopping the primary renderer:
adb shell am start -n dev.vkrender.renderer/.MainActivity -f 0x20000000 \
  --es displayOutputs none
```

IDs are Android logical display IDs, not physical SurfaceFlinger IDs. Lists are
comma-separated. `displayCrops` accepts semicolon-separated `id=x,y,width,height`
entries; shell-quote semicolons. Coordinates are normalized to the primary Vulkan
scene image. A configuration update replaces the selected IDs/crops; omission of
crop selects the full scene. Invalid crops are logged and ignored. The primary
Activity's own display is never selected as a secondary output.

`displayBufferSizes`, such as `4=1280x720`, optionally sets SurfaceView buffer
resolution separately from the physical output size. Android scales that surface
to the display. Sizes must be positive and at most 8192 per dimension; device
allocation limits can be lower. Omitting this extra preserves the previous size
configuration; a fresh Activity uses its display/layout dimensions.

## Ownership and synchronization

- UI callbacks acquire an `ANativeWindow` reference and enqueue attach/detach,
  crop, or surface-change requests. Superseded requests release their references.
- Only the renderer thread creates/destroys Vulkan surfaces and swapchains.
  Outputs share the primary device and queue; no second Vulkan device is created.
- The scene is rendered once. A device-local image per existing frame slot receives
  a GPU copy, then per-output GPU blits crop/scale it. There is no CPU pixel mapping
  or readback in this path. This is **GPU-only, not zero-copy**.
- Staging image reuse is protected by the existing primary frame fence. Output
  acquire semaphores are per frame slot; present-ready semaphores are per acquired
  swapchain image, so a render fence alone is not used to infer present completion.
- Acquires have zero timeout: an unavailable secondary skips that scene frame
  instead of waiting for an output image. FIFO presentation can still affect
  shared-queue scheduling; this is not independent/genlocked output pacing.
- Explicit detach, surface resize/loss, and shutdown quiesce the shared queue before
  destroying presentation resources. Crop-only changes retain the swapchain.
  Surface-change callbacks force recreation even when Android's currently connected
  buffer geometry still reports the previous size.
- Primary pause/stop dismisses secondary presentations; resume reattaches selected
  displays. A foreground primary Activity remains required.

## Control integration hooks

`MainActivity.displayControlReady()` reports a running Activity host.
`configureDisplayOutput(id, enabled, x, y, width, height)` validates a non-primary
presentation display and crop, then queues work on the UI thread. `true` means
**accepted**, not presented. The service's Binder adapter can use these hooks.

`getDisplayOutputsStatus()` returns bounded platform display metadata and desired
selection/crop, UI `surfaceReady`, and a separate `native` array of live swapchains
with dimensions, presented count and unavailable-image count. Native counters
refresh on first presentation and every 60 presentations; they are not scanout
fences or optical measurements. Lifecycle updates publish immediately.

## Validation and limitations

The Pi's two physical 1920x1080 outputs were exercised with the V3D driver.
The checked-in `test/android_display_pixels.py` pauses an otherwise fresh scene,
compares primary/secondary captures and restores the default crop and scene clock.
It requires Pillow/numpy and physical IDs from `dumpsys SurfaceFlinger --display-id`.
Raw logs, pixel results, final crop and negative-experiment images are in
[android-displays-20261029](android-displays-20261029).

- Full-frame output: byte-exact comparison in the primary's unobscured interior.
- Center-half crop scaled to full secondary: every RGB pixel byte-exact.
- Native secondary swapchain resize 1920x1080 → 1280x720 → 1920x1080 passed.
- Explicit detach/reattach repeated three times; HOME/resume recreated primary and
  secondary surfaces and resumed successful presentations.
- Tests are compositor capture checks, **not optical scanout or tearing validation**.
- No claim of independent display scenes, HDR/color-management conversion, Pi HWC
  tearing correction, sustained throughput qualification, or arbitrary hot-unplug
  behavior across other drivers.

### Preserved negative experiment

Direct cropped/scaled `vkCmdBlitImage` from the primary swapchain produced no
visible writes on the tested Pi path (a diagnostic magenta clear remained visible),
while full-size copy worked. Source/destination blit capabilities and rectangle
bounds were checked; a separate review found no concrete rectangle/barrier defect.
A GPU copy into a normal device-local staging image before scaling produced the
verified byte-exact result. This establishes the working path, **not a proven
root-cause diagnosis of the driver**. The diagnostic clear was removed from the
implementation. Staging adds GPU bandwidth and two retained scene-sized images.
