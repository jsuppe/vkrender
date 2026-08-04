package dev.vkrender.renderer;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.view.Surface;

import dev.vkrender.IVkRendererService;

/**
 * Bridges the native renderer's per-client AImageReader Surfaces to producer
 * apps over Binder. Exported behind the signature-level BIND_RENDERER
 * permission. Each calling app (identified by Binder UID) gets its own
 * stream, composited onto the cube - up to one per face (M11).
 */
public class RendererService extends Service {
    static {
        System.loadLibrary("vkrender_renderer");
    }

    private static native Surface nativeAcquireInputSurface(int uid);

    private static native void nativeReleaseInputSurface(int uid);

    private final IVkRendererService.Stub binder = new IVkRendererService.Stub() {
        @Override
        public Surface acquireInputSurface() {
            return nativeAcquireInputSurface(Binder.getCallingUid());
        }

        @Override
        public void releaseInputSurface() {
            // Tears down the caller's stream: its slots are freed and its
            // face(s) fall back to the remaining streams.
            nativeReleaseInputSurface(Binder.getCallingUid());
        }
    };

    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }
}
