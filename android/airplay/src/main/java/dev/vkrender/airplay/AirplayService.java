package dev.vkrender.airplay;

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
import android.provider.Settings;
import android.util.Log;
import android.view.Surface;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;

import dev.vkrender.IVkRendererService;

/**
 * M18: AirPlay receiver client app. Binds the renderer app's RendererService
 * for a Surface (like the decoder client), starts the ported UxPlay raop
 * server decoding into it, and advertises _raop._tcp/_airplay._tcp through
 * NsdManager using the TXT records the native side builds. iPhones then see
 * this device under Screen Mirroring, and the mirror lands on the scene.
 */
public class AirplayService extends Service {
    static {
        System.loadLibrary("vkrender_airplay");
    }

    private static final String TAG = "vkrender";
    private static final String CHANNEL_ID = "vkrender.airplay";
    private static final String RENDERER_PACKAGE = "dev.vkrender.renderer";
    private static final String RENDERER_SERVICE = "dev.vkrender.renderer.RendererService";

    private native boolean nativeStart(Surface surface, String name, byte[] hwaddr,
                                       String keyfile);
    private native String[] nativeGetService(int which); // [name, port, k=v...]
    private native boolean nativeSurfaceDead();
    private native void nativeStop();

    private Thread worker;
    private volatile boolean stopping = false;
    private final List<NsdManager.RegistrationListener> registrations = new ArrayList<>();

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        startAsForeground();
        if (worker == null) {
            worker = new Thread(this::sessionLoop, "vkr-airplay");
            worker.setDaemon(true);
            worker.start();
        }
        return START_STICKY;
    }

    private void sessionLoop() {
        while (!stopping) {
            Surface surface = acquireRendererSurface();
            if (surface == null) {
                Log.i(TAG, "airplay: renderer app not reachable, retrying");
                sleepMs(1000);
                continue;
            }
            byte[] hwaddr = stableHwAddr();
            String keyfile = getFilesDir().getAbsolutePath() + "/airplay.keys";
            if (!nativeStart(surface, "vkrender", hwaddr, keyfile)) {
                Log.e(TAG, "airplay: native start failed");
                sleepMs(2000);
                continue;
            }
            registerNsd();
            Log.i(TAG, "airplay: receiver up - visible as 'vkrender' in Screen Mirroring");
            // Session runs until the renderer dies (surface dead) or we stop.
            while (!stopping && !nativeSurfaceDead() && rendererAlive) sleepMs(1000);
            unregisterNsd();
            nativeStop();
            Log.i(TAG, "airplay: session ended, re-acquiring surface");
            sleepMs(500);
        }
    }

    private volatile boolean rendererAlive = false;

    private Surface acquireRendererSurface() {
        final BlockingQueue<Surface> result = new ArrayBlockingQueue<>(1);
        ServiceConnection conn = new ServiceConnection() {
            @Override
            public void onServiceConnected(ComponentName name, IBinder service) {
                try {
                    IVkRendererService renderer = IVkRendererService.Stub.asInterface(service);
                    renderer.releaseInputSurface(); // drop any stale stream of ours
                    Surface s = renderer.acquireInputSurface();
                    if (s != null) {
                        rendererAlive = true;
                        result.offer(s);
                    }
                } catch (Exception e) {
                    Log.w(TAG, "airplay: acquireInputSurface failed: " + e);
                }
            }

            @Override
            public void onServiceDisconnected(ComponentName name) {
                Log.i(TAG, "airplay: renderer disconnected");
                rendererAlive = false;
            }
        };
        Intent intent = new Intent();
        intent.setComponent(new ComponentName(RENDERER_PACKAGE, RENDERER_SERVICE));
        if (!bindService(intent, conn, Context.BIND_AUTO_CREATE)) return null;
        try {
            Surface s = result.poll(5, TimeUnit.SECONDS);
            if (s == null) unbindService(conn);
            return s;
        } catch (InterruptedException e) {
            unbindService(conn);
            return null;
        }
    }

    /** Stable locally-administered MAC-like id derived from ANDROID_ID. */
    private byte[] stableHwAddr() {
        byte[] mac = new byte[6];
        try {
            String id = Settings.Secure.getString(getContentResolver(),
                    Settings.Secure.ANDROID_ID);
            byte[] digest = MessageDigest.getInstance("SHA-256")
                    .digest(id.getBytes("UTF-8"));
            System.arraycopy(digest, 0, mac, 0, 6);
        } catch (Exception e) {
            mac = new byte[] {0x02, 0x13, 0x37, 0x42, 0x42, 0x42};
        }
        mac[0] = (byte) ((mac[0] | 0x02) & ~0x01); // locally administered, unicast
        return mac;
    }

    private void registerNsd() {
        NsdManager nsd = (NsdManager) getSystemService(Context.NSD_SERVICE);
        String[] types = {"_raop._tcp", "_airplay._tcp"};
        for (int which = 0; which < 2; ++which) {
            String[] desc = nativeGetService(which);
            if (desc == null || desc.length < 2) continue;
            NsdServiceInfo info = new NsdServiceInfo();
            info.setServiceName(desc[0]);
            info.setServiceType(types[which]);
            info.setPort(Integer.parseInt(desc[1]));
            for (int i = 2; i < desc.length; ++i) {
                int eq = desc[i].indexOf('=');
                if (eq > 0)
                    info.setAttribute(desc[i].substring(0, eq), desc[i].substring(eq + 1));
            }
            NsdManager.RegistrationListener listener = new NsdManager.RegistrationListener() {
                @Override public void onServiceRegistered(NsdServiceInfo i) {
                    Log.i(TAG, "airplay: registered " + i.getServiceType() + " as '"
                            + i.getServiceName() + "'");
                }
                @Override public void onRegistrationFailed(NsdServiceInfo i, int err) {
                    Log.e(TAG, "airplay: NSD registration failed: " + err);
                }
                @Override public void onServiceUnregistered(NsdServiceInfo i) {}
                @Override public void onUnregistrationFailed(NsdServiceInfo i, int err) {}
            };
            nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, listener);
            registrations.add(listener);
        }
    }

    private void unregisterNsd() {
        NsdManager nsd = (NsdManager) getSystemService(Context.NSD_SERVICE);
        for (NsdManager.RegistrationListener l : registrations) {
            try {
                nsd.unregisterService(l);
            } catch (Exception ignored) {
            }
        }
        registrations.clear();
    }

    private static void sleepMs(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException ignored) {
        }
    }

    private void startAsForeground() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID, "AirPlay receiver", NotificationManager.IMPORTANCE_LOW);
        nm.createNotificationChannel(channel);
        Notification notification = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("vkrender AirPlay")
                .setContentText("Receiving as 'vkrender'")
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
        unregisterNsd();
        nativeStop();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
