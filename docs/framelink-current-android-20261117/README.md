# Android functional UI validation — 2026-11-17

**PASS within the bounded scope below.** TCL 8483A, Android 15, Mali-G52 MC2.
FrameLink pin `b3b67a5fff798f8f72da5c0a4606bcf5dc49d00a`; renderer starting
source `a6a19061360761729db41bfa427f364037985b1a` plus the dependency-wiring
changes in this commit. No application playback defaults or synchronization
implementation changed. The installed APK matches the dual-ABI build receipt:
SHA256 `d9a9faae91d36c2704753fb5bb4f70ec38d3d01c0e339dbd1de6f94072b4c687`.

## Passed checks

- arm64-v8a and x86_64 builds, renderer APK assembly.
- Prior real-device GPU capability and RGBA pixel fixture: 23 frames, zero byte
  error. Native GPU SYNC_FD readiness/consumption and generic eventfd controls
  pass. These unchanged native results were reused, not rerun during UI batches.
- FrameLink input in a separate app process: 150 submitted frames, 150 logged
  adoptions across rate intervals. Full-screen captures were visually reviewed:
  changing cube orientation/color and moving white bars on the rendered scene.
- Real Binder/Surface hardware-Canvas producer: hidden 120-frame progress,
  moderation controls, asynchronous bounded JPEG preview with no idle readback,
  pause/resume availability and bounds, and stream removal.
- Tracked Surface release/reacquire and stale-owner release protection; renderer
  remains alive following intentional producer-process death.
- Legacy Surface release/reacquire; 20 rapid release/reacquire generations.
- Screen timeout restored after both batches; app data preserved. User-requested
  stay-awake-while-plugged-in setting is not reverted by these tests.

**Skipped:** secondary display controls, because none is eligible/attached.
This is not endurance, camera/decoder interoperability, arbitrary device support,
zero-copy end-to-end, optical scanout or acoustic synchronization qualification.
Android evidence does not establish fresh Windows/Linux compositor support.

## Preserved failure and repair

The initial UI attempt stopped at `renderer not focused`. Its runner queried
`dumpsys window windows`, which omits the global focus header on this build.
The full dump confirmed both current focus and resumed Activity were vkrender.
The runner now uses the full dump while storing only the focus line; it still
refuses screen capture when another app has focus. No app change or assertion
relaxation was needed. Original failed receipt: `initial-focus-failure.json`.

The Android emulator native fence failure is retained separately. Before any
FrameLink handoff, poll reported the exported sync_file readable while its ioctl
status was active and Vulkan queue completion was not ready. The physical tablet
raw fence remained unreadable until the event was released, and the original
contract passed. This localizes an emulator/native-fence-path issue, not its exact
driver cause, and adds no production workaround or gate waiver.

## Evidence and reproduction

`summary.json` states the accepted scope. Raw focused logs, build/native receipts,
JPEG preview and original failed receipt are adjacent. `artifact-manifest.json`
records original and normalized file hashes: only device serial/local home paths
are normalized in committed text. Full private raw outputs/screenshots remain in
the task workspace under `reports/vkrender-tablet-validation-20261117/`, with
`ui-complete/`, `lifecycle-complete/` and the original `validation/` directories.
Screenshot hashes and visual observations are in `visual-review.json`.

After verifying the installed APK and source pin, run serially on an unlocked
device (reusing recorded native diagnostics only when unchanged):

```sh
python3 scripts/validate_android_device_runtime.py --serial DEVICE_SERIAL \
  --apk /path/to/validated.apk --diagnostic /path/to/arm64/vkr_gpu_sync_fd_test \
  --out /path/to/new-ui-results --suite ui --skip-native
python3 scripts/validate_android_device_runtime.py --serial DEVICE_SERIAL \
  --apk /path/to/validated.apk --diagnostic /path/to/arm64/vkr_gpu_sync_fd_test \
  --out /path/to/new-lifecycle-results --suite lifecycle --skip-native
```

These are separate bounded batches, not scheduled/recovery jobs. They fail on
fixture errors and do not remove credentials, alter device security, or touch
Windows/Linux runtime configuration.
