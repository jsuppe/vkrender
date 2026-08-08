package dev.vkrender.rtsp;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/**
 * Thin launcher: starts the RTSP receiver service and finishes.
 *
 * <p>Forwards the {@code url} / {@code clear} extras so the source list can be
 * managed from adb without a UI:
 *
 * <pre>
 *   adb shell am start -n dev.vkrender.rtsp/.MainActivity -e url rtsp://cam/stream
 *   adb shell am start -n dev.vkrender.rtsp/.MainActivity -e clear true
 * </pre>
 */
public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Intent service = new Intent(this, RtspService.class);
        Intent launch = getIntent();
        if (launch != null) {
            String url = launch.getStringExtra("url");
            if (url != null) service.putExtra("url", url);
            // adb sends extras as strings, so accept "true" as well as a bool.
            String clear = launch.getStringExtra("clear");
            if (clear != null) service.putExtra("clear", Boolean.parseBoolean(clear));
            else if (launch.getBooleanExtra("clear", false)) service.putExtra("clear", true);
        }
        startForegroundService(service);
        finish();
    }
}
