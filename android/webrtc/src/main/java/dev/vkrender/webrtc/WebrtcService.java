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
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;

import dev.vkrender.IVkRendererService;

/**
 * M20: WebRTC (WHIP) receiver client app. Binds the renderer app's Surface
 * (like the decoder/airplay clients) and runs the whipserversrc pipeline
 * decoding the WHIP-ingested H.264 into it. A browser or any WHIP client
 * posts an SDP offer to http://<tablet-ip>:8889/whip/endpoint and streams.
 */
public class WebrtcService extends Service {
    static {
        System.loadLibrary("vkrender_webrtc");
    }

    private static final String TAG = "vkrender";
    private static final String CHANNEL_ID = "vkrender.webrtc";
    private static final String RENDERER_PACKAGE = "dev.vkrender.renderer";
    private static final String RENDERER_SERVICE = "dev.vkrender.renderer.RendererService";
    private static final int WHIP_PORT = 8889;

    private native boolean nativeStart(Surface surface, int port);
    private native boolean nativeSurfaceDead();
    private native void nativeStop();

    private static final String WHIP_SERVICE_TYPE = "_vkrender-whip._tcp";

    private Thread worker;
    private volatile boolean stopping = false;
    private volatile boolean rendererAlive = false;

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
            Surface surface = acquireRendererSurface();
            if (surface == null) {
                Log.i(TAG, "webrtc: renderer app not reachable, retrying");
                sleepMs(1000);
                continue;
            }
            if (!nativeStart(surface, WHIP_PORT)) {
                Log.e(TAG, "webrtc: native start failed");
                nativeStop();
                sleepMs(2000);
                continue;
            }
            Log.i(TAG, "webrtc: WHIP receiver up on port " + WHIP_PORT
                    + " - POST an SDP offer to http://<this-ip>:" + WHIP_PORT + "/whip/endpoint");
            advertise();
            while (!stopping && !nativeSurfaceDead() && rendererAlive) sleepMs(1000);
            nativeStop();
            Log.i(TAG, "webrtc: session ended, re-acquiring surface");
            sleepMs(500);
        }
    }

    private Surface acquireRendererSurface() {
        final BlockingQueue<Surface> result = new ArrayBlockingQueue<>(1);
        ServiceConnection conn = new ServiceConnection() {
            @Override
            public void onServiceConnected(ComponentName name, IBinder service) {
                try {
                    IVkRendererService renderer = IVkRendererService.Stub.asInterface(service);
                    renderer.releaseInputSurface();
                    Surface s = renderer.acquireInputSurface();
                    if (s != null) {
                        rendererAlive = true;
                        result.offer(s);
                    }
                } catch (Exception e) {
                    Log.w(TAG, "webrtc: acquireInputSurface failed: " + e);
                }
            }

            @Override
            public void onServiceDisconnected(ComponentName name) {
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
                .setContentText("WHIP endpoint on port " + WHIP_PORT)
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
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
