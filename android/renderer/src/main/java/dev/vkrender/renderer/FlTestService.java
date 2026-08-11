// M40: a framelink test PRODUCER in its own process (":fltest").
//
// It exists because of where Android draws its sandbox lines: SELinux MLS
// categories confine abstract unix sockets to processes of the SAME app, so a
// producer in another APK cannot reach the renderer's slot channels - Binder
// (the AIDL Surface API) is the blessed cross-app path, which is exactly why
// that API stays. framelink on Android is the native-code path for producers
// living inside the same application: a capture thread, a decoder process, a
// plugin the app hosts itself.
//
// This service is that case in miniature - a second PROCESS of the renderer
// app, attaching to vkrender.in.N over the socket like any framelink producer.
// Start it from adb:
//
//   adb shell am start-foreground-service \
//     -n dev.vkrender.renderer/.FlTestService --ei frames 300
//
// (start-service works too on a foreground app; frames defaults to 300.)
package dev.vkrender.renderer;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

public class FlTestService extends Service {
    static {
        System.loadLibrary("vkrender_renderer");
    }

    // Runs the produce loop; returns frames actually submitted.
    private static native int nativeProduce(String channel, int frames);

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        final int frames = intent != null ? intent.getIntExtra("frames", 300) : 300;
        final String channel =
                intent != null && intent.getStringExtra("channel") != null
                        ? intent.getStringExtra("channel")
                        : "vkrender.in.0";
        new Thread(() -> {
            Log.i("vkrender", "fltest: producing " + frames + " frames on '" + channel + "'");
            int n = nativeProduce(channel, frames);
            Log.i("vkrender", "fltest: submitted " + n + " frames");
            stopSelf();
        }, "fl-test-producer").start();
        return START_NOT_STICKY;
    }
}
