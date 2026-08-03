package dev.vkrender;

import android.app.NativeActivity;
import android.content.Intent;
import android.os.Bundle;

/**
 * Renderer process: a NativeActivity whose native loop (android_main.cpp)
 * does the Vulkan work. On create it launches the DecoderService - which the
 * manifest places in a separate OS process (":decoder") - passing through the
 * RTSP url from our own intent.
 *
 * Launch:
 *   adb shell am start -n dev.vkrender/.MainActivity \
 *       --es url rtsp://10.0.2.2:8554/test
 * (10.0.2.2 is the emulator's alias for the host machine.)
 */
public class MainActivity extends NativeActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        String url = getIntent().getStringExtra("url");
        if (url == null) url = "rtsp://10.0.2.2:8554/test";
        Intent svc = new Intent(this, DecoderService.class);
        svc.putExtra("url", url);
        startForegroundService(svc);
        super.onCreate(savedInstanceState);
    }
}
