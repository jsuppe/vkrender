package dev.vkrender.airplay;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/** Thin launcher: starts the AirPlay receiver service and finishes. */
public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        startForegroundService(new Intent(this, AirplayService.class));
        finish();
    }
}
