package dev.vkrender.renderer;

import android.app.NativeActivity;

/**
 * The renderer app: a NativeActivity whose native loop (android_main.cpp)
 * does the Vulkan work - AImageReader input, zero-copy AHardwareBuffer
 * import, video on a spinning cube. Producers are OTHER APPS (M10): they
 * bind RendererService for the input Surface. This activity starts nothing.
 *
 * Launch:
 *   adb shell am start -n dev.vkrender.renderer/.MainActivity
 */
public class MainActivity extends NativeActivity {
    @Override public void onCreate(android.os.Bundle savedInstanceState) {
        // Set before NativeActivity loads the native library: trace flags are cached.
        boolean trace = getIntent().getBooleanExtra("trace", false);
        try {
            String prefix = new java.io.File(getFilesDir(), "performance").getAbsolutePath();
            android.system.Os.setenv("VKR_TRACE", trace ? prefix + "-renderer" : "", true);
            android.system.Os.setenv("FRAMELINK_TRACE", trace ? prefix + "-framelink" : "", true);
            android.system.Os.setenv("VKR_TRACE_GPU", trace && getIntent().getBooleanExtra("traceGpu", false) ? "1" : "0", true);
            String layout=getIntent().getStringExtra("layout");
            android.system.Os.setenv("VKR_LAYOUT",layout==null?"two-cubes":layout,true);
            String inputFormats=getIntent().getStringExtra("inputFormats");
            android.system.Os.setenv("VKR_INPUT_FORMATS",inputFormats==null?"":inputFormats,true);
            String outputMode=getIntent().getStringExtra("outputMode");
            android.system.Os.setenv("VKR_OUTPUT_MODE",outputMode==null?"cpu":outputMode,true);
            String outputs = getIntent().getStringExtra("sceneOutputs");
            android.system.Os.setenv("VKR_SCENE_OUTPUTS", outputs == null ? "" : outputs, true);
            int frames = trace ? Math.max(0, getIntent().getIntExtra("traceFrames", 0)) : 0;
            android.system.Os.setenv("VKR_TRACE_FRAMES", Integer.toString(frames), true);
        } catch (android.system.ErrnoException e) {
            android.util.Log.e("vkrender", "Cannot configure performance capture", e);
        }
        super.onCreate(savedInstanceState);
    }
}
