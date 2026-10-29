# Android Binder Surface lifetime

The four original AIDL methods retain their transaction order and behavior.
All five packaged copies of `IVkRendererService.aidl` also append:

- `acquireInputSurfaceWithOwner(int slot, IBinder owner)`
- `releaseInputSurfaceWithOwner(int slot, IBinder owner)`

New producers should keep a process-local `Binder` alive for their stream and
pass it to both methods. The renderer links a death recipient to that token.
A different token replaces the previous UID/slot generation. Repeated acquire
with the same token is idempotent. A queued death callback or explicit release
from a superseded token cannot release the replacement. Null tokens are rejected
by these opt-in methods. UID isolation is based on Binder's actual calling UID.
The owner token is a lifetime signal, not an authorization credential.

Legacy acquire relinquishes token tracking for that slot and returns its existing
reader. Legacy release remains unconditional for the calling UID/slot. Legacy
clients therefore still require explicit release; crash detection is not promised
for clients that have not opted in. No client applications are automatically migrated.

Native release detaches the active reader on the render thread. Reacquire can
then allocate a new reader while the previous generation's last image remains
visible during the Presentation layout's departure animation. Each generation
has its own scene identity. Other layouts have no departure animation, but still
retain resources through the GPU fence. Reader, current image, imported image
and descriptors are destroyed only after an ensuing successful submission's
fence retires. Ordinary release no longer calls `vkDeviceWaitIdle`.

Image replacement also parks the old image behind the **new successful
submission**, not the older frame fence that was just waited. Swapchain acquisition
failures keep pending images retained until a submission succeeds or shutdown
waits for the device. JNI owns a window reference through Java Surface conversion.

## Validation

- `python3 test/android_stream_owners.py`: real Java ownership implementation,
  host Binder contract stubs; replacement, queued stale death, explicit stale
  release, already-dead token, UID/slot isolation, legacy takeover, failed acquire,
  and service cleanup.
- `vkr_android_surface_generations_test`: concurrent old/new generation tiles,
  departure retains old texture, last-stream exit clears cached texture.
- Android arm64 native renderer build with NDK 28.2.13676358 passed.
- Android Gradle `:renderer:compileDebugJavaWithJavac` including AIDL passed.

## Physical Pi validation (2026-10-29)

Android Pi, V3D 7.1.7.0, Presentation layout, separate debug producer process:

- Explicit tracked release/immediate reacquire and stale-owner release passed.
- Genuine producer process death caused native stream departure and GPU-fenced
  retirement without an explicit release (tested twice).
- Legacy acquire/release/reacquire passed.
- Activity HOME/resume recreated the native presentation surface while the
  producer was running; rendering/reacquisition/death cleanup subsequently passed.
- Twenty rapid generations, three HardwareCanvas frames per generation, passed.
- Last-stream departure completed and imports returned to zero. In total,
  28 generations were retired across the two renderer process runs.
- Logs show real AHardwareBuffer imports, frame adoptions and presentations,
  not only successful Binder calls. There were no test FAIL/AndroidRuntime errors.

Raw logs are in [android-binder-lifecycle-20261029](android-binder-lifecycle-20261029).
The debug-only `SurfaceLifecycleTestService` is absent from release APKs. Run with
`am startservice -n dev.vkrender.renderer/.SurfaceLifecycleTestService`, optionally
`--ez legacy true` or `--ez rapid true`, while the renderer Activity is foreground.
The default tracked test intentionally kills only its own separate producer process.

This is functional lifecycle validation, not pixel/scanout correctness or a
Vulkan-validation-layer run. Sustained high-churn/memory-growth testing and a
second physical driver remain unqualified. Cuttlefish's currently available
Vulkan 1.1 image cannot qualify this renderer. The previous foreground virtual
camera demo task was restored after testing; the tested debug renderer is installed
but stopped. Its original APK backup is retained under `/tmp/vkrender-binder-device`.
