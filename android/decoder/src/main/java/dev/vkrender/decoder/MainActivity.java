package dev.vkrender.decoder;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/**
 * Thin launcher for the decoder client app: forwards the RTSP url to the
 * foreground DecoderService and finishes. The service does the real work
 * (bind the renderer app, get its Surface, decode into it).
 *
 * Launch:
 *   adb shell am start -n dev.vkrender.decoder/.MainActivity \
 *       --es url rtsp://127.0.0.1:8556/test
 */
public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        String url = getIntent().getStringExtra("url");
        if (url == null) url = "rtsp://10.0.2.2:8556/test";
        Intent svc = new Intent(this, DecoderService.class);
        svc.putExtra("url", url);
        startForegroundService(svc);
        finish();
    }
}
