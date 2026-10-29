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
    private static native String nativeControlStatus();
    private static native String nativeControlCapabilities();
    private static native int nativeControl(String action, int slot, String value);

    private final StreamOwners<Surface> owners = new StreamOwners<>(new StreamOwners.Backend<Surface>() {
        public Surface acquire(int uid, int slot) { return nativeAcquireInputSurface(uid, slot); }
        public void release(int uid, int slot) { nativeReleaseInputSurface(uid, slot); }
    });

    private final IVkRendererService.Stub binder = new IVkRendererService.Stub() {
        @Override public int getControlVersion() { return 1; }
        @Override public String getControlCapabilities() { return nativeControlCapabilities(); }
        @Override public String getControlStatus() { return nativeControlStatus(); }
        @Override public int submitControl(String action, int slot, String value) {
            if(action==null || value==null || action.length()>64 || value.length()>256)return 1;
            return nativeControl(action,slot,value);
        }

        @Override
        public Surface acquireInputSurface() {
            return owners.acquire(Binder.getCallingUid(), 0, null);
        }

        @Override
        public void releaseInputSurface() {
            // Tears down the caller's slot-0 stream: its slots are freed and
            // its face(s) fall back to the remaining streams.
            owners.release(Binder.getCallingUid(), 0, null);
        }

        @Override
        public Surface acquireInputSurfaceSlot(int slot) {
            return owners.acquire(Binder.getCallingUid(), slot, null);
        }

        @Override
        public void releaseInputSurfaceSlot(int slot) {
            owners.release(Binder.getCallingUid(), slot, null);
        }
        @Override
        public Surface acquireInputSurfaceWithOwner(int slot, IBinder owner) {
            if (owner == null) throw new IllegalArgumentException("owner is required");
            return owners.acquire(Binder.getCallingUid(), slot, owner);
        }

        @Override
        public void releaseInputSurfaceWithOwner(int slot, IBinder owner) {
            if (owner == null) throw new IllegalArgumentException("owner is required");
            owners.release(Binder.getCallingUid(), slot, owner);
        }
    };

    @Override public void onDestroy() {
        owners.close();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }
}
