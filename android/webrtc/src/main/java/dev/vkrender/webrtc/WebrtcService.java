package dev.vkrender.webrtc;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.ServiceInfo;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;
import android.view.Surface;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;

import dev.vkrender.IVkRendererService;

/**
 * M22: WHIP room client app. Binds the renderer and holds up to 12 slot
 * Surfaces (acquireInputSurfaceSlot) - one per presenter. The native side
 * (whip_room on webrtcbin) accepts concurrent WHIP sessions and calls back up
 * here for a slot Surface per presenter track, so each presenter lands on
 * their own cube face. Any WHIP sender works: the Flutter app, a browser,
 * OBS, whipclientsink.
 */
public class WebrtcService extends Service {
    static {
        System.loadLibrary("vkrender_webrtc");
    }

    private static final String TAG = "vkrender";
    private static final String CHANNEL_ID = "vkrender.webrtc";
    private static final String RENDERER_PACKAGE = "dev.vkrender.renderer";
    private static final String RENDERER_SERVICE = "dev.vkrender.renderer.RendererService";
    private static final String WHIP_SERVICE_TYPE = "_vkrender-whip._tcp";
    private static final int WHIP_PORT = 8889;
    private static final int MAX_PRESENTERS = 12;

    private native boolean nativeStart(int port);
    private native void nativePoll();
    private native void nativeStop();

    private Thread worker;
    private volatile boolean stopping = false;
    private volatile boolean rendererAlive = false;
    private volatile IVkRendererService renderer;
    private ServiceConnection rendererConn;

    // sessionId -> renderer slot, guarded by `this`.
    private final Map<Integer, Integer> sessionSlots = new HashMap<>();
    private final boolean[] slotUsed = new boolean[MAX_PRESENTERS];

    private NsdManager nsdManager;
    private NsdManager.RegistrationListener regListener;
    private volatile boolean advertised = false;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        startAsForeground();
        if (worker == null) {
            worker = new Thread(this::sessionLoop, "vkr-webrtc");
            worker.setDaemon(true);
            worker.start();
        }
        return START_STICKY;
    }

    private void sessionLoop() {
        while (!stopping) {
            if (!bindRenderer()) {
                Log.i(TAG, "webrtc: renderer app not reachable, retrying");
                sleepMs(1000);
                continue;
            }
            if (!nativeStart(WHIP_PORT)) {
                Log.e(TAG, "webrtc: native start failed");
                nativeStop();
                unbindRenderer();
                sleepMs(2000);
                continue;
            }
            Log.i(TAG, "webrtc: WHIP room up on port " + WHIP_PORT
                    + " - presenters POST offers to http://<this-ip>:" + WHIP_PORT
                    + "/whip/endpoint");
            advertise();
            // Poll: reaps departed presenters. If the renderer app dies, every
            // slot Surface is abandoned (codecs stall silently - M10 lesson),
            // so tear the room down and rebuild once it's back.
            while (!stopping && rendererAlive) {
                nativePoll();
                sleepMs(500);
            }
            nativeStop();
            clearSlots();
            unbindRenderer();
            Log.i(TAG, "webrtc: renderer lost, rebuilding room");
            sleepMs(500);
        }
    }

    // ---- called from native (whip_room callbacks) --------------------------

    /** Acquire a renderer slot Surface for a presenter session. */
    public synchronized Surface acquireSlotSurface(int sessionId) {
        IVkRendererService r = renderer;
        if (r == null) return null;
        int slot = -1;
        for (int i = 0; i < MAX_PRESENTERS; i++) {
            if (!slotUsed[i]) {
                slot = i;
                break;
            }
        }
        if (slot < 0) return null;
        try {
            Surface s = r.acquireInputSurfaceSlot(slot);
            if (s == null) return null;
            slotUsed[slot] = true;
            sessionSlots.put(sessionId, slot);
            Log.i(TAG, "webrtc: session " + sessionId + " -> renderer slot " + slot);
            return s;
        } catch (Exception e) {
            Log.w(TAG, "webrtc: acquireInputSurfaceSlot failed: " + e);
            return null;
        }
    }

    /** Presenter left: free their renderer slot. */
    public synchronized void releaseSlotSurface(int sessionId) {
        Integer slot = sessionSlots.remove(sessionId);
        if (slot == null) return;
        slotUsed[slot] = false;
        IVkRendererService r = renderer;
        if (r == null) return;
        try {
            r.releaseInputSurfaceSlot(slot);
        } catch (Exception e) {
            Log.w(TAG, "webrtc: releaseInputSurfaceSlot failed: " + e);
        }
    }

    private synchronized void clearSlots() {
        sessionSlots.clear();
        for (int i = 0; i < MAX_PRESENTERS; i++) slotUsed[i] = false;
    }

    // ---- renderer binding --------------------------------------------------

    private boolean bindRenderer() {
        final BlockingQueue<Boolean> ready = new ArrayBlockingQueue<>(1);
        rendererConn = new ServiceConnection() {
            @Override
            public void onServiceConnected(ComponentName name, IBinder service) {
                renderer = IVkRendererService.Stub.asInterface(service);
                rendererAlive = true;
                ready.offer(Boolean.TRUE);
            }

            @Override
            public void onServiceDisconnected(ComponentName name) {
                renderer = null;
                rendererAlive = false;
            }
        };
        Intent intent = new Intent();
        intent.setComponent(new ComponentName(RENDERER_PACKAGE, RENDERER_SERVICE));
        try {
            if (!bindService(intent, rendererConn, Context.BIND_AUTO_CREATE)) return false;
        } catch (SecurityException e) {
            // Signature-permission mismatch (renderer + room APKs must be
            // signed with the same key, e.g. both from one CI run). Retry
            // loop stays alive instead of crash-looping the service.
            Log.e(TAG, "webrtc: bind rejected - are both APKs signed alike? " + e);
            rendererConn = null;
            return false;
        }
        try {
            Boolean ok = ready.poll(5, TimeUnit.SECONDS);
            if (ok == null) {
                unbindRenderer();
                return false;
            }
            return true;
        } catch (InterruptedException e) {
            unbindRenderer();
            return false;
        }
    }

    private void unbindRenderer() {
        renderer = null;
        rendererAlive = false;
        if (rendererConn != null) {
            try {
                unbindService(rendererConn);
            } catch (Exception ignored) {
            }
            rendererConn = null;
        }
    }

    // ---- mDNS --------------------------------------------------------------

    /** Advertise _vkrender-whip._tcp so the Flutter sender auto-discovers us. */
    private void advertise() {
        if (advertised) return;
        NsdServiceInfo info = new NsdServiceInfo();
        info.setServiceName("vkrender");
        info.setServiceType(WHIP_SERVICE_TYPE);
        info.setPort(WHIP_PORT);
        info.setAttribute("path", "/whip/endpoint");
        nsdManager = getSystemService(NsdManager.class);
        regListener = new NsdManager.RegistrationListener() {
            @Override
            public void onServiceRegistered(NsdServiceInfo info) {
                Log.i(TAG, "webrtc: mDNS registered " + info.getServiceName());
            }
            @Override
            public void onRegistrationFailed(NsdServiceInfo info, int errorCode) {
                Log.w(TAG, "webrtc: mDNS registration failed: " + errorCode);
            }
            @Override
            public void onServiceUnregistered(NsdServiceInfo info) {}
            @Override
            public void onUnregistrationFailed(NsdServiceInfo info, int errorCode) {}
        };
        try {
            nsdManager.registerService(info, NsdManager.PROTOCOL_DNS_SD, regListener);
            advertised = true;
        } catch (Exception e) {
            Log.w(TAG, "webrtc: mDNS register threw: " + e);
        }
    }

    private void unadvertise() {
        if (advertised && nsdManager != null && regListener != null) {
            try {
                nsdManager.unregisterService(regListener);
            } catch (Exception ignored) {
            }
        }
        advertised = false;
    }

    // ---- service scaffolding ----------------------------------------------

    private static void sleepMs(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException ignored) {
        }
    }

    private void startAsForeground() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID, "WebRTC receiver", NotificationManager.IMPORTANCE_LOW);
        nm.createNotificationChannel(channel);
        Notification notification = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("vkrender WebRTC")
                .setContentText("WHIP room on port " + WHIP_PORT)
                .setSmallIcon(android.R.drawable.ic_media_play)
                .setOngoing(true)
                .build();
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(1, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        } else {
            startForeground(1, notification);
        }
    }

    @Override
    public void onDestroy() {
        stopping = true;
        unadvertise();
        nativeStop();
        unbindRenderer();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
