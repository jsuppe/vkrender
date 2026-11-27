# Current FrameLink dependency

## 2026-11-26: pin moved to FrameLink master `0d72a94b`

Pin: `0d72a94b7fb0610bcaa284c0198314916b876862` (FrameLink `master`, PR #46
merge), still protocol 6. Renderer base: vkrender `master` `b650398`, which
brings in fullscreen across displays (PR #15) and the scene-output fix (PR #20).

Changes since `b3b67a5f` are additive: no public symbol is removed. Windows
gains only the `FL_VULKAN_IMPORT` flag, which it accepts and ignores. Linux and
Android gain `framelink_vulkan.h`, consumer pool snapshots and the M6.9/M6.8
source fixes. The Windows API and behaviour match the frozen `060c54e4`.

- Linux (local, NVIDIA): 238/238 targets built, CTest 22/22 passed, none skipped.
  No compositor playback run.
- Hosted CI (run 45898770499): Linux 15/15 and Windows 13/13 GPU-free tests,
  Android APKs (renderer, decoder, airplay, webrtc) and sender 15/15 all pass.
  CI now reads `cmake/framelink-revision.txt` instead of its own revision.
- Android, TCL 8483A (Android 15, Mali-G52 MC2), `validate_android_framelink.py`
  with Gradle 8.14: arm64-v8a and x86_64 builds, renderer APK, and on-device
  GPU capability, SYNC_FD ready/consumed (PASS) and RGBA output pixels
  (23 frames, worst byte error 0) all pass. Two earlier attempts stopped at the
  APK step: one used Gradle 8.5 (AGP needs 8.7), and one failed
  `validateSigningDebug` without a recorded cause. That failure did not recur
  when the task ran alone or in the third, passing run.
- Windows, RTX 5090 test host, 2026-11-26, from vkrender master `1afa005`
  (PR #23 merge), FrameLink `0d72a94b` and framecam `5dd1a7a`. All are fresh
  clones built with `cmake --preset ci` under the Windows validation mutex:
  - `ctest --preset hardware` 5/5 and `ctest --preset ci` 13/13 passed.
  - `nv12_service.ps1` NV12 and BGRA each passed (30/30 distinct frames).
    `parity_windows_input.ps1` passed (40 frames).
  - `windows_gpu_stress_pixels.ps1 -Frames 600`, BGRA and `-Rgb10`: both
    600/600 matched, worst YUV error 1 (limit 3), 0 sequence gaps, arrival
    p99 18.2-18.3 ms.
  - The scripts need a standalone FrameLink preset build at the pin for
    `-FrameLinkBuild` (`fl_produce.exe`, `gstframelink.dll`). Run each script
    in its own PowerShell process: `parity_windows_input.ps1` sets
    `VKR_INPUT_FORMATS=nv12`, and that setting breaks a later BGRA pixel run in
    the same session.
- Not re-run at this pin: the tablet UI/Binder/Surface lifecycle checks and the
  emulator checks below (recorded at `b3b67a5f`), and Linux compositor
  playback. vkrender has no renderer-level 1080p60/4K30 rate harness. Those
  cells exist only in FrameLink's transport harness, so the renderer is not
  qualified at those targets.

## 2026-11-17: pin `b3b67a5f`


Renderer starting point: `a6a19061360761729db41bfa427f364037985b1a`, the M6
renderer baseline. The legacy `vkrender-src` tree and its unrelated edits are not
modified. FrameLink source pin: `b3b67a5fff798f8f72da5c0a4606bcf5dc49d00a`.
The pin is local source, not an assertion that this revision is published remotely.

Desktop and Android share a revision/clean-source check before adding FrameLink.
There are no default, queue-size, playback, format or GPU synchronization changes.
Android disables FrameLink's optional GStreamer targets; the renderer remains a
native Vulkan/Surface app. The existing Gradle `framelinkDir` property selects the
checkout and both Android ABIs use the same pin.

## Validation status

- Revision-check controls: arm64 Android configuration accepts the exact pin;
  the old protocol-v5 checkout and a missing checkout are both rejected.
- Native Android arm64-v8a and x86_64 compilation: pass.
- Renderer APK assembly: pass; SHA256
  `d9a9faae91d36c2704753fb5bb4f70ec38d3d01c0e339dbd1de6f94072b4c687`.
- Physical Android GPU capability, RGBA output pixels and native SYNC_FD ready/
  consumed checks: pass on TCL 8483A, Android 15, Mali-G52 MC2.
- Physical tablet UI/control checks: pass, including FrameLink animated input,
  real Surface/Binder input, moderation, bounded JPEG preview and pause/resume.
  The first attempt's focus failure was in the runner's dumpsys subsection;
  the renderer was actually focused. Full-dump focus inspection repairs the
  runner without modifying or relaxing any application assertion.
- Surface lifecycle checks: see the [collected Android evidence](framelink-current-android-20261117/README.md).
- Android 15 x86_64 emulator: functional UI/lifecycle checks pass; native SYNC_FD
  timing still fails. Raw FD polling reports readability before Vulkan queue
  completion and while SYNC_IOC_FILE_INFO still says active, before FrameLink
  receives the FD. Generic FrameLink eventfd wait/reuse controls pass. Physical
  hardware does not reproduce the early readiness. No production workaround or
  fence-gate waiver was added; exact emulator/driver cause remains unresolved.
- Fresh Windows integrated renderer qualification: not run by this update.
  Linux validation was attempted but stopped at development dependency discovery,
  before build or compositor playback. Neither platform is newly qualified.
  Historical Windows `a50841a` results do not qualify the new source pair.

The new Windows retained-device-owner API is additive. This update does not
migrate the renderer to it, qualify repeated Windows reconnect, or expand format
support. Android cross-UID authorization still requires the existing Binder path
or a separately integrated authorized FrameLink broker; named socket channels do
not bypass Android sandbox restrictions.

Loom pre-edit briefing was attempted in this session; its CLI is unavailable
because the installed environment lacks `chromadb`. No requirements store was
modified. This file records the bounded update and its evidence instead.

Manual serial validator:

```sh
python3 scripts/validate_android_framelink.py \
  --framelink-dir /path/to/pinned/framelink --out /path/to/new/evidence \
  --gradle /path/to/gradle --serial DEVICE_SERIAL
```

It builds both Android ABIs and the APK, then optionally runs the existing native
GPU capability, sync-FD and RGBA output-pixel fixtures. Exit 77 is recorded as a
skip, never a pass. It does not install/replace the renderer APK, launch a full
compositor session, run the checker without its required peer, or touch Windows.
