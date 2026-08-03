package dev.vkrender;

/**
 * The renderer's producer-facing API: hands out the Surface of its
 * AImageReader. Any Surface producer (MediaCodec, camera, another app if the
 * service is exported) can feed video into the renderer's Vulkan scene -
 * BufferQueue provides the queue/fence/timestamp plumbing, zero-copy.
 */
interface IVkRendererService {
    /** The renderer's input Surface, or null if the renderer isn't up yet. */
    Surface acquireInputSurface();

    /** Signals the producer is done with the surface. */
    void releaseInputSurface();
}
