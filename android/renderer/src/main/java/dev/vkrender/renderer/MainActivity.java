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
    private volatile boolean controlResumed;
    private volatile long controlEpoch;
    private static volatile MainActivity displayHost;
    private final java.util.concurrent.atomic.AtomicInteger displayPending=new java.util.concurrent.atomic.AtomicInteger();
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
            // FrameLink A/V input (6a, FrameLink decision 12; APKs built with
            // VKR_GST_ANDROID_ROOT). The session directory is under files/.
            String avSession = getIntent().getStringExtra("avSession");
            android.system.Os.setenv("VKR_AV_SESSION", avSession == null ? "" : avSession, true);
            String avFormat = getIntent().getStringExtra("avFormat");
            android.system.Os.setenv("VKR_AV_FORMAT", avFormat == null ? "NV12" : avFormat, true);
            android.system.Os.setenv("VKR_AV_WIDTH", Integer.toString(getIntent().getIntExtra("avWidth", 1920)), true);
            android.system.Os.setenv("VKR_AV_HEIGHT", Integer.toString(getIntent().getIntExtra("avHeight", 1080)), true);
            android.system.Os.setenv("VKR_AV_FPS", Integer.toString(getIntent().getIntExtra("avFps", 60)), true);
            android.system.Os.setenv("VKR_AV_LATENCY_MS", Integer.toString(getIntent().getIntExtra("avLatencyMs", 0)), true);
            android.system.Os.setenv("VKR_AV_MEDIA_RUN_MS", Integer.toString(getIntent().getIntExtra("avMediaRunMs", 0)), true);
            android.system.Os.setenv("VKR_AV_EXIT_ON_END", getIntent().getBooleanExtra("avExitOnEnd", false) ? "1" : "0", true);
            if (trace && avSession != null) {
                String avPrefix = new java.io.File(getFilesDir(), avSession).getAbsolutePath();
                android.system.Os.setenv("VKR_TRACE", avPrefix + "-renderer", true);
                android.system.Os.setenv("FRAMELINK_TRACE", avPrefix + "-transport", true);
                android.system.Os.setenv("VKR_TRACE_EVENTS", "524288", true);
                android.system.Os.setenv("FRAMELINK_TRACE_EVENTS", "262144", true);
            }
        } catch (android.system.ErrnoException e) {
            android.util.Log.e("vkrender", "Cannot configure performance capture", e);
        }
        super.onCreate(savedInstanceState);
        displayOutputs=new DisplayOutputs(this);
        displayOutputs.configure(getIntent());
        displayHost=this;
    }
    @Override protected void onResume() {super.onResume();++controlEpoch;controlResumed=true;}
    @Override protected void onPause() {controlResumed=false;++controlEpoch;super.onPause();}
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
        MainActivity host=displayHost;return host!=null&&host.displayOutputs!=null&&host.controlResumed&&host.displayOutputs.ready();
    }
    public static String getDisplayOutputsStatus() {
        MainActivity host=displayHost;
        return host==null||host.displayOutputs==null?"{\"ready\":false,\"displays\":[],\"native\":[]}":host.displayOutputs.status().replace("\"ready\":true","\"ready\":"+displayControlReady());
    }
    public static int configureDisplayOutputResult(int id,boolean enabled,float x,float y,float width,float height) {
        if(!Float.isFinite(x)||!Float.isFinite(y)||!Float.isFinite(width)||!Float.isFinite(height)||
           x<0||y<0||width<=0||height<=0||x+width>1||y+height>1)return 1;
        MainActivity host=displayHost;
        if(host==null||!displayControlReady())return 3;
        if(!host.displayOutputs.eligible(id))return 1;
        long epoch=host.controlEpoch;
        for(;;){int pending=host.displayPending.get();if(pending>=128)return 4;
            if(host.displayPending.compareAndSet(pending,pending+1))break;}
        host.runOnUiThread(()->{try {
            // Discard queued work after a host/lifecycle transition.
            if(displayHost==host&&host.controlResumed&&host.displayOutputs.ready()&&host.controlEpoch==epoch)host.displayOutputs.setOutput(id,enabled,x,y,width,height);
        }finally{host.displayPending.decrementAndGet();}});
        return 0; // accepted, not necessarily presented: inspect the native status
    }
    public static boolean configureDisplayOutput(int id,boolean enabled,float x,float y,float width,float height) {
        return configureDisplayOutputResult(id,enabled,x,y,width,height)==0;
    }
    @Override protected void onDestroy() {
        if(displayHost==this)displayHost=null;
        super.onDestroy();
    }
}
