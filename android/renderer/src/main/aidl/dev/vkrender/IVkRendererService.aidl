package dev.vkrender;

/**
 * The renderer's producer-facing API: hands out the Surface of its
 * AImageReader. Any Surface producer (MediaCodec, camera, another app - the
 * service is exported behind a signature permission) can feed video into the
 * renderer's Vulkan scene - BufferQueue provides the queue/fence/timestamp
 * plumbing, zero-copy.
 *
 * NOTE: the decoder app carries a copy of this file; the AIDL package
 * (dev.vkrender) is the Binder interface descriptor and must match exactly.
 */
interface IVkRendererService {
    /** The renderer's input Surface, or null if the renderer isn't up yet. */
    Surface acquireInputSurface();

    /** Signals the producer is done with the surface. */
    void releaseInputSurface();
}
