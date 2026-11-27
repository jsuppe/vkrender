// FrameLink decision 12 (6a): FrameLink's A/V producer in its own process
// (":avproducer") of the renderer app, for the identity captures. Only in APKs
// built with the A/V input (VKR_GST_ANDROID_ROOT). Start it from adb after the
// renderer Activity is running with an avSession extra:
//
//   adb shell am start-foreground-service -n dev.vkrender.renderer/.AvProducerService \
//     --es session av-session --es format NV12 --ei width 1920 --ei height 1080 \
//     --ei fps 60 --ei seconds 5
//
// The session directory is relative to the app's files directory; the
// producer's stderr goes to files/<session>-producer.log.
package dev.vkrender.renderer;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;
import java.io.File;

public class AvProducerService extends Service {
    private static native int nativeRun(String[] args, String logPath);

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) return START_NOT_STICKY;
        final String session = intent.getStringExtra("session") != null ? intent.getStringExtra("session") : "av-session";
        final String format = intent.getStringExtra("format") != null ? intent.getStringExtra("format") : "NV12";
        final int width = intent.getIntExtra("width", 1920), height = intent.getIntExtra("height", 1080);
        final int fps = intent.getIntExtra("fps", 60), seconds = intent.getIntExtra("seconds", 5);
        final File dir = new File(getFilesDir(), session);
        final String[] args = {
            "static", "--session", dir.getAbsolutePath(), "--format", format,
            "--frames", Integer.toString(fps * seconds), "--width", Integer.toString(width),
            "--height", Integer.toString(height), "--fps", Integer.toString(fps),
            "--timeout-ms", "15000", "--media-run-ms", Integer.toString(seconds * 1000)};
        final String log = new File(getFilesDir(), session + "-producer.log").getAbsolutePath();
        // Producer-side traces (6a): transport and GStreamer element, beside the
        // renderer's, before the library loads (trace settings are cached).
        if (intent.getBooleanExtra("trace", false)) {
            try {
                String prefix = new File(getFilesDir(), session).getAbsolutePath();
                android.system.Os.setenv("FRAMELINK_TRACE", prefix + "-ptransport", true);
                android.system.Os.setenv("FRAMELINK_GST_TRACE", prefix + "-pgst", true);
                android.system.Os.setenv("FRAMELINK_TRACE_EVENTS", "262144", true);
                android.system.Os.setenv("FRAMELINK_GST_TRACE_EVENTS", "262144", true);
            } catch (android.system.ErrnoException e) {
                Log.e("vkrender", "avproducer: cannot configure tracing", e);
            }
        }
        new Thread(() -> {
            try {
                System.loadLibrary("vkr_av_producer");
            } catch (UnsatisfiedLinkError e) {
                Log.e("vkrender", "avproducer: this APK was built without the A/V input", e);
                stopSelf();
                return;
            }
            Log.i("vkrender", "avproducer: " + format + " " + width + "x" + height + "@" + fps + " for " + seconds + " s");
            int rc = nativeRun(args, log);
            Log.i("vkrender", "avproducer: exit " + rc);
            stopSelf();
            // The producer's GStreamer state is process-wide; one run per process.
            // exit() (not killProcess) so the trace recorders flush at teardown.
            System.exit(0);
        }, "fl-av-producer").start();
        return START_NOT_STICKY;
    }
}
