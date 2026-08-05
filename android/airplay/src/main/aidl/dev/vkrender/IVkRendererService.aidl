package dev.vkrender;

/**
 * Client copy of the renderer app's Binder interface (see the renderer
 * module's aidl/ for the canonical comments). The AIDL package (dev.vkrender)
 * is the Binder interface descriptor and must match the service exactly.
 */
interface IVkRendererService {
    /** The renderer's input Surface, or null if the renderer isn't up yet. */
    Surface acquireInputSurface();

    /** Signals the producer is done with the surface. */
    void releaseInputSurface();
}
