package dev.vkrender;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;
import android.view.Surface;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;

/**
 * Decoder process host (android:process=":decoder", foreground service).
 * M8 architecture: binds the renderer's RendererService over Binder, receives
 * its input Surface, and hands it to the native RTSP+MediaCodec pipeline -
 * which simply decodes INTO that surface. BufferQueue carries the frames
 * cross-process; there is no custom frame IPC anymore.
 */
public class DecoderService extends Service {
    static {
        System.loadLibrary("vkrender_native");
    }

    private static final String TAG = "vkrender";
    private static final String CHANNEL_ID = "vkrender.decoder";

    /** Runs one decode session into the surface; returns when it is unusable. */
    private native void nativeRunSession(String url, Surface surface);

    private Thread worker;
    private volatile boolean stopping = false;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        startAsForeground();
        if (worker == null) {
            final String url = intent != null ? intent.getStringExtra("url") : null;
            worker = new Thread(() -> sessionLoop(url != null ? url : ""), "vkr-decoder");
            worker.setDaemon(true);
            worker.start();
        }
        return START_STICKY;
    }

    /**
     * Bind -> acquire surface -> decode session -> on failure (renderer died,
     * surface abandoned) unbind and retry.
     */
    private void sessionLoop(String url) {
        while (!stopping) {
            Surface surface = acquireRendererSurface();
            if (surface == null) {
                sleepMs(1000);
                continue;
            }
            Log.i(TAG, "decoder: got renderer surface, starting session");
            nativeRunSession(url, surface);
            Log.i(TAG, "decoder: session ended, re-acquiring surface");
            sleepMs(500);
        }
    }

    private Surface acquireRendererSurface() {
        final BlockingQueue<Surface> result = new ArrayBlockingQueue<>(1);
        ServiceConnection conn = new ServiceConnection() {
            @Override
            public void onServiceConnected(ComponentName name, IBinder service) {
                try {
                    Surface s = IVkRendererService.Stub.asInterface(service).acquireInputSurface();
                    if (s != null) result.offer(s);
                } catch (Exception e) {
                    Log.w(TAG, "acquireInputSurface failed: " + e);
                }
            }

            @Override
            public void onServiceDisconnected(ComponentName name) {}
        };
        Intent intent = new Intent(this, RendererService.class);
        if (!bindService(intent, conn, Context.BIND_AUTO_CREATE)) return null;
        try {
            Surface s = result.poll(5, java.util.concurrent.TimeUnit.SECONDS);
            if (s == null) unbindService(conn);
            // Keep the binding alive for the session's duration when we got a
            // surface; the connection object leaks per session, which is fine
            // for the retry cadence involved.
            return s;
        } catch (InterruptedException e) {
            unbindService(conn);
            return null;
        }
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
                CHANNEL_ID, "Stream decoder", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Keeps the RTSP decoder running");
        nm.createNotificationChannel(channel);

        Notification notification = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("vkrender")
                .setContentText("Decoding stream")
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
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
