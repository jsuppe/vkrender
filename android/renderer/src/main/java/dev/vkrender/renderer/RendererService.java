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
 * permission. Streams are keyed (calling UID, slot): classically one stream
 * per app (slot 0, M11), and since M22 one app may hold several slots - the
 * WHIP room APK fans up to 12 presenters into separate cube faces.
 */
public class RendererService extends Service {
    static {
        System.loadLibrary("vkrender_renderer");
    }

    private static native Surface nativeAcquireInputSurface(int uid, int slot);

    private static native void nativeReleaseInputSurface(int uid, int slot);

    private final IVkRendererService.Stub binder = new IVkRendererService.Stub() {
        @Override
        public Surface acquireInputSurface() {
            return nativeAcquireInputSurface(Binder.getCallingUid(), 0);
        }

        @Override
        public void releaseInputSurface() {
            // Tears down the caller's slot-0 stream: its slots are freed and
            // its face(s) fall back to the remaining streams.
            nativeReleaseInputSurface(Binder.getCallingUid(), 0);
        }

        @Override
        public Surface acquireInputSurfaceSlot(int slot) {
            return nativeAcquireInputSurface(Binder.getCallingUid(), slot);
        }

        @Override
        public void releaseInputSurfaceSlot(int slot) {
            nativeReleaseInputSurface(Binder.getCallingUid(), slot);
        }
    };

    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }
}
