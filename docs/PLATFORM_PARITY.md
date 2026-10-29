# Cross-platform parity — implementation ledger (2026-10-29)

Scope: all functional gaps across FrameLink and vkrender, not tracing alone.
This ledger is in-progress, not a blanket parity certification.

## Implemented in FrameLink

- Portable `flImageMapPlanes` in the core header; Linux native mapping,
  Windows staging readback/upload, Android native mapping or normalization.
- BGRA, RGBA, NV12 and RGB10A2 allocation paths. Device allocation support
  still matters: Pi Android rejected RGB10A2, tablet and desktop accepted it.
- Android NV12 maps native YUV420 directly when U/V layout is verified as NV12;
  otherwise normalizes planes. Android BGRA maps through native RGBA swizzling.
  AHB handles always expose their native format, not a promise of NV12 bytes.
- Android producer/consumer samples and static GStreamer plugin build.
- Native POSIX completion-FD handoff (`framelink_sync_fd.h`), with bounded
  ready waits, GPU-owned readiness and deferred consumption watermarks.
- `FL_DEFER_RELEASE`: wait for readiness normally but retire storage explicitly
  after GPU reads. Windows uses its existing shared consumed timeline fence.
- Windows now honors ready-fence acquisition timeouts, reserves acquired slots,
  and checks held-frame release identity. Android held-frame resize is refused;
  refused Android resize preserves producer counters and outstanding ownership.
- Android API tracing matches desktop operation/metadata coverage.

**Wire version is now 5.** Rebuild both ends together; old peers are refused.
Native handle APIs are necessarily platform-specific, with equivalent ownership
and completion contracts. Android abstract sockets remain subject to app UID /
SELinux boundaries; renderer cross-app input uses its existing Binder Surface API.

## Renderer work

- Shared CPU/GPU tracing helper and Android intent-based finite captures.
- Linux/Android scene outputs through persistent asynchronous Vulkan readback
  and CPU conversion. Reuses existing frame fence waits. Opt-in fallback,
  not zero-copy and not performance-equivalent to Windows GPU video processing.
- Per-format sampler/pipeline variants support mixed RGB/NV12 native inputs.
  Linux native mixed DMA-BUF pixel test verifies RGB gradient and NV12 red tiles.
- Linux/Android FrameLink recycling now uses explicit deferred release behind
  completed GPU fences instead of fabricating an older flFrame.
- `VKR_INPUT_FORMATS=nv12,rgba,...` chooses per-slot formats. Desktop non-BGRA
  FrameLink inputs use an explicit CPU conversion/upload fallback. Android uses
  AHB format-aware import. Runtime verification of all combinations is ongoing.

## Validation recorded so far

- Linux FrameLink: 24/24 tests, including format maps, ownership and FD contracts.
- Windows FrameLink: 27/27 tests including RGB10A2 GStreamer and ready-fence timeout.
  Includes final resize/GPU-lifetime guard changes.
- Pi + tablet Android: RGBA/NV12/BGRA byte tests, completion-FD tests and GStreamer
  NV12 padded/offset-plane test passed. Final deferred resize regression passed on both devices.
- Linux + Pi dual scene outputs: 20 timestamp-matched RGB/NV12 frame pairs,
  >=17 distinct frames, maximum BT.709 YUV error 1 (limit 3).
- Pi trace capture: 180 GPU duration records, zero trace-buffer loss.
- Windows renderer: 3/3 GPU conversion tests and 60 distinct frames on each
  comparison path passed after mixed-sampler/deferred-release refactors.

## Remaining evaluation / qualification

- Long-running GPU-owned reconnect and process-death stress across all backends.
- Remaining input format combinations beyond verified Linux/Windows NV12 and
  Android mixed native NV12/RGBA.
- Native GPU completion integration now passed Linux, Pi and tablet using
  exported Vulkan SYNC_FD semaphores gated by real queued event waits.
- Android now shares all four layout calculations with desktop, and supports
  L/Space/B/-/=/1..9/0 keyboard controls. Multi-display/control-service parity
  remain open. FrameLink animated departure retention is now implemented;
  Binder Surface lifecycle parity still needs separate qualification.
- GPU-only scene outputs are now implemented on Linux and capable Android
  devices. Linux uses compute/native plane copies; Android YUV uses external-format
  resolve. Pi native NV12 GPU rendering remains unsupported by its current driver.
  Windows keeps its GPU implementation; these are functionally similar, not
  identical performance paths. See the renderer GPU_OUTPUT.md for qualification.
- Android cross-app transport requires Binder or platform privileges; arbitrary
  cross-app Unix-socket access cannot be enabled by this library.

## Android GStreamer build

```sh
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DFRAMELINK_BUILD_GST=ON \
  -DFRAMELINK_GST_ANDROID_ROOT=/path/to/gstreamer-android/arm64
cmake --build build-android
```

Link static `gstframelink` into the APK and register with
`GST_PLUGIN_STATIC_REGISTER(framelink)` after GStreamer initialization.
The SDK is an external dependency, not vendored.

## Additional verified paths

- GStreamer RGB10A2_LE padded/offset byte roundtrip and incompatible-format
  rejection: Linux, Windows and Android tablet. Pi allocation unsupported.
- Android native NV12 input: 90 submitted / 87 adopted in the measured run.
- Android mixed native NV12/RGBA wall: both producers submitted 120 frames;
  screenshot inspected, both distinct patterned tiles visible. This fixture is
  not an RGB-vs-YUV color-equivalence or no-drop benchmark.
- Shared scene-layout unit test covers all four layouts, portrait/landscape/wide
  canvases and mixed landscape/portrait source aspect ratios.

Android launch extras: `--es layout wall`, `--es inputFormats nv12,rgba`,
`--es sceneOutputs output.rgba,output.nv12`, `--ez trace true`,
`--ez traceGpu true`, `--ei traceFrames 300`. Restart the process to change
launch configuration. Keyboard controls may be sent with `adb shell input keyevent`.

Windows output extension: native Vulkan blit targets now include RGBA and RGB10A2
as well as BGRA; NV12 keeps D3D11 video processing. Both RGBA/NV12 and
RGB10A2/NV12 timestamp-matched output checks passed 20 distinct pairs with
maximum YUV error 1. This renders an 8-bit scene into 10-bit storage; it does
not create additional HDR scene precision. Windows NV12 input fallback rendered
40 frames and its red fixture center decoded to RGB (255,1,0).

Producer/consumer samples now exercise explicit completion mode on all three
platforms. POSIX CPU validation reports completed reads/writes with FD -1; the
separate Vulkan test proves real native GPU fence transfer.

Reproduce Windows integration (isolated build directories, not the live demo):

```powershell
./test/parity_windows_input.ps1 -RendererBuild build-trace -FrameLinkBuild ../framelink/build-trace
./test/parity_windows_output.ps1 -RendererBuild build-trace -FrameLinkBuild ../framelink/build-trace
./test/parity_windows_output.ps1 -RendererBuild build-trace -FrameLinkBuild ../framelink/build-trace -Rgb10
```

The input script checks finite renderer completion; inspect `parity-nvinput.ppm`
for its red fixture. The output script checks 20 timestamp-matched frame pairs,
frame diversity and YUV conversion error. These scripts stop only their own
child processes, not an existing live demo.

## Reconnect lifetime regression (2026-10-29)

Replacement producers now receive `FL_BUSY` while old storage is held or its
completion fence is pending. A successful reconnect starts a fresh generation;
old completion values cannot authorize reuse in the new timeline. Renderer
departure paths retire their final retained frame after GPU reads complete.

- FrameLink suites: Linux 25/25, Windows 28/28 passed.
- Held-frame reconnect and delayed-completion reconnect passed on Linux and
  both Android devices; held-frame reconnect also passed on Windows.
- Renderer NV12 reconnect: Linux completed 30 adoptions; Windows completed
  70 rendered frames across reconnect; Pi producer submitted 90 frames with
  one reconnect and no producer timeouts (renderer trace: 89 adoptions).
- Linux CPU-fallback 640x360 integration exceeded its 15-second test timeout;
  the passing reconnect fixture used 128x72 input. This is not a throughput
  qualification. Long-running GPU/process-death stress remains outstanding.

### Android FrameLink departure animation

Presentation tiles retain their imported buffer during exit motion, then signal
final consumption after the existing GPU frame fence. Other layouts retire
immediately behind the fence. Switching layouts clears stale motion/cache entries.
The shared layout now advances exits when the last live stream disappears.

Validation: Linux and Windows shared-layout tests cover last-stream exit and
layout-switch cleanup. Pi Android presentation-mode NV12 producer submitted
120 frames across one reconnect with zero producer timeouts; renderer logged
departure retirement before replacement imports. This does not qualify Binder
Surface disconnect animations or Android physical multi-display.

## GPU-output follow-up

Opt-in FL_GPU_RENDER allocation and strict renderer GPU output mode added.
Linux renderer suite 9/9; FrameLink Linux25/25 and Windows28/28. Windows existing
GPU conversion3/3 and60 distinct frames per RGB/NV12 comparison path still pass.
Native patterned output tests include scaling, chroma, row padding and consumer
recreation/resizing while an output is pending: Linux/tablet4 formats, Pi2 formats
passed with zero byte error. Pi NV12 and RGB10 allocation limits are explicit.
Tablet full Activity comparison remains unverified while screen-locked; native
conversion/lifecycle tests passed. Broad platform integration parity is not complete.

## Linux control-plane implementation

Linux `--wayland` now honors `--control-port` (default 8875, 0 disables HTTP),
using the same loopback-only HTTP server and moderation policy as Windows.
`GET /api/capabilities` reports available camera, preview and display controls.
Linux has no virtual-camera gate: `/api/camera` returns 501, not false success.
Runtime display attach/detach is available in headless mode when GLFW can access
a display server; unsupported display control returns 501. Preview reads the
actual last primary image, in both windowed and headless modes.

FrameLink input slots are 0..11; live Wayland content surfaces receive slots
12..23, stable for the lifetime of the surface and reusable after disconnect.
Use the current `/api/streams` roster before issuing a command. Kicking a Wayland
surface disconnects its entire client (and hence all that client's surfaces).
Hidden/rotated-out inputs are still acquired/released and Wayland frame callbacks
continue; moderation does not stall their producer rings. Explicit kick retires
GPU reads before closing FrameLink storage and removes retained presentation tiles.
Stream `fps` is mean adoption rate since connection, not display scanout FPS;
`ageMs` measures time since the last adopted frame.

All platforms now validate layout names, boolean toggles, integer slot IDs,
known roster/display IDs, max-visible bounds and finite rotation intervals
(1..86400 seconds). Unknown stream actions return 400; command queue saturation
returns 503. These reject formerly silently ignored malformed commands.

CPU-only tests `shared_moderation` and `control_http` cover selection/rotation,
hide/pin/spotlight interplay, HTTP commands and malformed/unsupported requests.

Validation: Linux CPU suite 4/4 (IPC, shared layouts, moderation, HTTP) passed.
Live GPU/control integration passed with concurrent Wayland and FrameLink inputs;
a hidden FrameLink producer completed 240 frames with zero timeouts. JPEG/MJPEG
preview, Wayland kick, all moderation actions and unsupported camera rejection
passed. Preview capture is queued before present while the image remains acquired,
not a read of an already-presented swapchain image. Physical display hotplug and
windowed preview need separate display-equipped qualification.

Windows now uses the same pre-present preview capture and the actual acquired
image index instead of reading image 0 after present. The existing demand-driven
readback wait remains; no preview readback occurs without an HTTP request. Linux
integration was repeated with Khronos validation enabled: passed with no reported
Vulkan validation errors. New pure control tests are included in Linux/Windows CI.
