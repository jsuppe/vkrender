package dev.vkrender;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/**
 * Decoder process host: the manifest runs this Service in its own process
 * (android:process=":decoder"), so the RTSP + MediaCodec pipeline lives a
 * true process boundary away from the renderer. All real work happens in
 * native code (decoder_jni.cpp -> MediaCodecDecoder).
 *
 * Runs as a foreground service: without it, the cached-app freezer suspends
 * this process the moment the activity leaves the foreground (screen off),
 * killing the stream mid-flight.
 */
public class DecoderService extends Service {
    static {
        System.loadLibrary("vkrender_native");
    }

    private static final String CHANNEL_ID = "vkrender.decoder";

    private native void nativeRun(String url);
    private Thread worker;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        startAsForeground();
        if (worker == null) {
            final String url = intent != null ? intent.getStringExtra("url") : null;
            worker = new Thread(() -> nativeRun(url != null ? url : ""), "vkr-decoder");
            worker.setDaemon(true);
            worker.start();
        }
        return START_STICKY;
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
    public IBinder onBind(Intent intent) {
        return null;
    }
}
