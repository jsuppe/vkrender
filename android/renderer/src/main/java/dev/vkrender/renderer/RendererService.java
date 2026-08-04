package dev.vkrender.renderer;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.view.Surface;

import dev.vkrender.IVkRendererService;

/**
 * Bridges the native renderer's AImageReader Surface to producer apps over
 * Binder. Exported behind the signature-level BIND_RENDERER permission. The
 * native side creates the reader at startup (android_main);
 * nativeAcquireInputSurface blocks briefly until it exists.
 */
public class RendererService extends Service {
    static {
        System.loadLibrary("vkrender_renderer");
    }

    private static native Surface nativeAcquireInputSurface();

    private final IVkRendererService.Stub binder = new IVkRendererService.Stub() {
        @Override
        public Surface acquireInputSurface() {
            return nativeAcquireInputSurface();
        }

        @Override
        public void releaseInputSurface() {
            // The reader (and its buffers) belong to the renderer; producer
            // disconnect is handled by BufferQueue itself. Nothing to do yet.
        }
    };

    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }
}
