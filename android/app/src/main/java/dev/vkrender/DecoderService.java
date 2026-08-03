package dev.vkrender;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/**
 * Decoder process host: the manifest runs this Service in its own process
 * (android:process=":decoder"), so the RTSP + MediaCodec pipeline lives a
 * true process boundary away from the renderer. All real work happens in
 * native code (decoder_jni.cpp -> MediaCodecDecoder).
 */
public class DecoderService extends Service {
    static {
        System.loadLibrary("vkrender_native");
    }

    private native void nativeRun(String url);
    private Thread worker;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (worker == null) {
            final String url = intent != null ? intent.getStringExtra("url") : null;
            worker = new Thread(() -> nativeRun(url != null ? url : ""), "vkr-decoder");
            worker.setDaemon(true);
            worker.start();
        }
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
