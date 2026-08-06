package dev.vkrender;

/**
 * The renderer's producer-facing API: hands out the Surface of its
 * AImageReader. Any Surface producer (MediaCodec, camera, another app - the
 * service is exported behind a signature permission) can feed video into the
 * renderer's Vulkan scene - BufferQueue provides the queue/fence/timestamp
 * plumbing, zero-copy.
 *
 * M22: streams are keyed (calling UID, slot). The slot methods let ONE client
 * app hold several streams - the WHIP room APK fans up to 12 presenters into
 * separate cube faces. The legacy methods are slot 0. New methods are
 * appended so existing clients keep their Binder transaction codes.
 *
 * NOTE: every client app carries a copy of this file; the AIDL package
 * (dev.vkrender) is the Binder interface descriptor and must match exactly.
 */
interface IVkRendererService {
    /** The renderer's input Surface (slot 0), or null if not up yet. */
    Surface acquireInputSurface();

    /** Signals the producer is done with the slot-0 surface. */
    void releaseInputSurface();

    /** A per-slot input Surface, so one app can produce several streams. */
    Surface acquireInputSurfaceSlot(int slot);

    /** Tears down the caller's stream in `slot`. */
    void releaseInputSurfaceSlot(int slot);
}
