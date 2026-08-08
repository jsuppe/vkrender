package dev.vkrender.rtsp;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;
import android.view.Surface;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;

import dev.vkrender.IVkRendererService;

/**
 * M27: RTSP receiver client app. Binds the renderer and holds up to
 * MAX_SOURCES slot Surfaces (acquireInputSurfaceSlot) - one per camera. The
 * native side (rtsp_room) owns one rtspsrc pipeline per URL and calls back up
 * here for a slot Surface per source, so each camera lands on its own cube
 * face.
 *
 * <p>RTSP is PULL, which makes this the odd receiver out: there is nothing to
 * advertise over mDNS and no PIN (the camera authenticates us via credentials
 * in the URL). What it needs instead is a source list, so URLs are persisted
 * in SharedPreferences and can be managed three ways:
 *
 * <pre>
 *   adb shell am start -n dev.vkrender.rtsp/.MainActivity \
 *       -e url rtsp://user:pw@cam.local/stream
 *   adb shell am start -n dev.vkrender.rtsp/.MainActivity -e clear true
 *   curl -d rtsp://cam.local/stream http://&lt;device-ip&gt;:8890/sources
 * </pre>
 */
public class RtspService extends Service {
    static {
        System.loadLibrary("vkrender_rtsp");
    }

    private static final String TAG = "vkrender";
    private static final String CHANNEL_ID = "vkrender.rtsp";
    private static final String RENDERER_PACKAGE = "dev.vkrender.renderer";
    private static final String RENDERER_SERVICE = "dev.vkrender.renderer.RendererService";
    private static final String PREFS = "vkrender.rtsp";
    private static final String PREF_URLS = "urls";
    private static final int CONTROL_PORT = 8890;
    private static final int MAX_SOURCES = 8;

    private native boolean nativeStart(int controlPort, int maxSources);
    private native int nativeAddSource(String url);
    private native boolean nativeRemoveSource(int id);
    private native void nativePoll();
    private native String nativeListSources();
    private native void nativeStop();

    private Thread worker;
    private volatile boolean stopping = false;
    private volatile boolean rendererAlive = false;
    private volatile IVkRendererService renderer;
    private ServiceConnection rendererConn;
    private volatile boolean roomUp = false;

    // sourceId -> renderer slot, guarded by `this`.
    private final Map<Integer, Integer> sourceSlots = new HashMap<>();
    private final boolean[] slotUsed = new boolean[MAX_SOURCES];

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        startAsForeground();
        // Intent extras let adb (or another app) manage the source list
        // without a UI. Persisted so they survive a restart, since a camera
        // feed is meant to be always-on.
        if (intent != null) {
            if (intent.getBooleanExtra("clear", false)) {
                saveUrls(new LinkedHashSet<>());
                Log.i(TAG, "rtsp: source list cleared");
            }
            String url = intent.getStringExtra("url");
            if (url != null && !url.trim().isEmpty()) addUrl(url.trim());
        }
        if (worker == null) {
            worker = new Thread(this::sessionLoop, "vkr-rtsp");
            worker.setDaemon(true);
            worker.start();
        }
        return START_STICKY;
    }

    private void sessionLoop() {
        while (!stopping) {
            if (!bindRenderer()) {
                Log.i(TAG, "rtsp: renderer app not reachable, retrying");
                sleepMs(1000);
                continue;
            }
            if (!nativeStart(CONTROL_PORT, MAX_SOURCES)) {
                Log.e(TAG, "rtsp: native start failed");
                nativeStop();
                unbindRenderer();
                sleepMs(2000);
                continue;
            }
            roomUp = true;
            for (String url : loadUrls()) {
                int id = nativeAddSource(url);
                Log.i(TAG, "rtsp: seeded source " + id + " " + url);
            }
            Log.i(TAG, "rtsp: control API on http://<this-ip>:" + CONTROL_PORT + "/sources");
            // Poll: builds pending sources, runs backoff + stall watchdog. If
            // the renderer app dies every slot Surface is abandoned (codecs
            // stall silently - M10 lesson), so tear the room down and rebuild.
            while (!stopping && rendererAlive) {
                nativePoll();
                sleepMs(500);
            }
            roomUp = false;
            nativeStop();
            clearSlots();
            unbindRenderer();
            Log.i(TAG, "rtsp: renderer lost, rebuilding room");
            sleepMs(500);
        }
    }

    // ---- source list -------------------------------------------------------

    private SharedPreferences prefs() {
        return getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    private Set<String> loadUrls() {
        return new LinkedHashSet<>(prefs().getStringSet(PREF_URLS, new LinkedHashSet<>()));
    }

    private void saveUrls(Set<String> urls) {
        prefs().edit().putStringSet(PREF_URLS, urls).apply();
    }

    /** Persist a URL and, if the room is already up, start pulling it now. */
    private void addUrl(String url) {
        Set<String> urls = loadUrls();
        if (urls.add(url)) saveUrls(urls);
        if (roomUp) {
            int id = nativeAddSource(url);
            Log.i(TAG, "rtsp: added source " + id + " " + url);
        } else {
            Log.i(TAG, "rtsp: stored " + url + " (starts with the room)");
        }
    }

    // ---- called from native (rtsp_room callbacks) --------------------------

    /** Acquire a renderer slot Surface for a source. */
    public synchronized Surface acquireSlotSurface(int sourceId) {
        IVkRendererService r = renderer;
        if (r == null) return null;
        // A reconnecting source releases and re-acquires, so an existing
        // mapping means a stale slot we should reuse rather than leak.
        Integer existing = sourceSlots.get(sourceId);
        int slot = existing != null ? existing : -1;
        if (slot < 0) {
            for (int i = 0; i < MAX_SOURCES; i++) {
                if (!slotUsed[i]) {
                    slot = i;
                    break;
                }
            }
        }
        if (slot < 0) return null;
        try {
            Surface s = r.acquireInputSurfaceSlot(slot);
            if (s == null) return null;
            slotUsed[slot] = true;
            sourceSlots.put(sourceId, slot);
            Log.i(TAG, "rtsp: source " + sourceId + " -> renderer slot " + slot);
            return s;
        } catch (Exception e) {
            Log.w(TAG, "rtsp: acquireInputSurfaceSlot failed: " + e);
            return null;
        }
    }

    /** Source removed or reconnecting: free its renderer slot. */
    public synchronized void releaseSlotSurface(int sourceId) {
        Integer slot = sourceSlots.remove(sourceId);
        if (slot == null) return;
        slotUsed[slot] = false;
        IVkRendererService r = renderer;
        if (r == null) return;
        try {
            r.releaseInputSurfaceSlot(slot);
        } catch (Exception e) {
            Log.w(TAG, "rtsp: releaseInputSurfaceSlot failed: " + e);
        }
    }

    private synchronized void clearSlots() {
        sourceSlots.clear();
        for (int i = 0; i < MAX_SOURCES; i++) slotUsed[i] = false;
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
            // Signature-permission mismatch (renderer + receiver APKs must be
            // signed with the same key, e.g. both from one CI run).
            Log.e(TAG, "rtsp: bind rejected - are both APKs signed alike? " + e);
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
                CHANNEL_ID, "RTSP receiver", NotificationManager.IMPORTANCE_LOW);
        nm.createNotificationChannel(channel);
        Notification notification = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("vkrender RTSP")
                .setContentText("Pulling RTSP sources")
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
        roomUp = false;
        nativeStop();
        unbindRenderer();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
