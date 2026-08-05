package dev.vkrender.webrtc;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/** Thin launcher: starts the WebRTC receiver service and finishes. */
public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        startForegroundService(new Intent(this, WebrtcService.class));
        finish();
    }
}
