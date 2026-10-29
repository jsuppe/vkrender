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
    private DisplayOutputs displayOutputs;
    private static volatile MainActivity displayHost;
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
        displayOutputs=new DisplayOutputs(this);
        displayOutputs.configure(getIntent());
        displayHost=this;
    }
    @Override protected void onStart() {
        super.onStart();displayOutputs.start();
    }
    @Override protected void onStop() {
        displayOutputs.stop();super.onStop();
    }
    @Override protected void onNewIntent(android.content.Intent intent) {
        super.onNewIntent(intent);
        if(intent.hasExtra("displayOutputs")||intent.hasExtra("displayCrops")||intent.hasExtra("displayBufferSizes"))displayOutputs.configure(intent);
    }
    public static boolean displayControlReady() {
        MainActivity host=displayHost;return host!=null&&host.displayOutputs!=null&&host.displayOutputs.ready();
    }
    public static String getDisplayOutputsStatus() {
        MainActivity host=displayHost;
        return host==null||host.displayOutputs==null?"{\"ready\":false,\"displays\":[],\"native\":[]}":host.displayOutputs.status();
    }
    public static boolean configureDisplayOutput(int id,boolean enabled,float x,float y,float width,float height) {
        MainActivity host=displayHost;
        if(host==null||!displayControlReady()||!host.displayOutputs.eligible(id)||
           !Float.isFinite(x)||!Float.isFinite(y)||!Float.isFinite(width)||!Float.isFinite(height)||
           x<0||y<0||width<=0||height<=0||x+width>1||y+height>1)return false;
        host.runOnUiThread(()->host.displayOutputs.setOutput(id,enabled,x,y,width,height));
        return true; // accepted for UI/render-thread processing, not yet presented
    }
    @Override protected void onDestroy() {
        if(displayHost==this)displayHost=null;
        super.onDestroy();
    }
}
