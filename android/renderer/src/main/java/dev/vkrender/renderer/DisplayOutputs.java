package dev.vkrender.renderer;

import android.app.Activity;
import android.app.Presentation;
import android.content.Intent;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import java.util.HashMap;
import java.util.HashSet;

/** Optional physical/virtual display outputs, owned by the visible Activity.
 * Native code owns an acquired window reference independently of callbacks.
 */
final class DisplayOutputs implements DisplayManager.DisplayListener {
    static { System.loadLibrary("vkrender_renderer"); }
    private static native String nativeDisplayStatus();
    private static native void nativeSetDisplaySurface(int displayId,Surface surface,float x,float y,float width,float height,boolean recreate);
    private final Activity activity;
    private final DisplayManager manager;
    private final HashMap<Integer,Output> outputs=new HashMap<>();
    private final HashMap<Integer,float[]> crops=new HashMap<>();
    private final HashSet<Integer> enabled=new HashSet<>();
    private boolean all;
    private volatile boolean started;
    private volatile String snapshot="[]";
    private final HashMap<Integer,int[]> sizes=new HashMap<>();
    DisplayOutputs(Activity activity) {
        this.activity=activity;manager=activity.getSystemService(DisplayManager.class);
    }
    void configure(Intent intent) {
        enabled.clear();crops.clear();all=false;
        String ids=intent.getStringExtra("displayOutputs");
        if(ids!=null) {
            all=ids.equals("all");
            if(!all)for(String id:ids.split(","))try{enabled.add(Integer.parseInt(id.trim()));}catch(NumberFormatException ignored){}
        }
        String raw=intent.getStringExtra("displayCrops");
        if(raw!=null)for(String part:raw.split(";"))try {
            String[] pair=part.split("=");String[] values=pair[1].split(",");
            if(values.length!=4)throw new IllegalArgumentException();
            float[] c=new float[4];for(int i=0;i<4;i++)c[i]=Float.parseFloat(values[i]);
            for(float v:c)if(!Float.isFinite(v))throw new IllegalArgumentException();
            if(c[0]<0||c[1]<0||c[2]<=0||c[3]<=0||c[0]+c[2]>1||c[1]+c[3]>1)throw new IllegalArgumentException();
            crops.put(Integer.parseInt(pair[0]),c);
        } catch(Exception error){Log.w("vkrender","Invalid display crop: "+part);}
        String rawSizes=intent.getStringExtra("displayBufferSizes");
        if(rawSizes!=null) {
            sizes.clear();
            for(String part:rawSizes.split(";"))try {
                String[] pair=part.split("=");String[] wh=pair[1].split("x");
                int w=Integer.parseInt(wh[0]),h=Integer.parseInt(wh[1]);
                if(w<=0||h<=0||w>8192||h>8192)throw new IllegalArgumentException();
                sizes.put(Integer.parseInt(pair[0]),new int[]{w,h});
            }catch(Exception error){Log.w("vkrender","Invalid display buffer size: "+part);}
        }
        if(started)refresh();
    }
    void start() {
        if(started)return;started=true;
        manager.registerDisplayListener(this,new Handler(Looper.getMainLooper()));refresh();
    }
    void stop() {
        started=false;manager.unregisterDisplayListener(this);
        for(Output output:outputs.values())output.dismiss();outputs.clear();updateSnapshot();
    }
    private boolean selected(Display display) {
        int primary=activity.getWindowManager().getDefaultDisplay().getDisplayId();
        return display.getDisplayId()!=primary&&(all||enabled.contains(display.getDisplayId()));
    }
    private void refresh() {
        for(Integer id:new HashSet<>(outputs.keySet())) {
            Display display=manager.getDisplay(id);
            if(display==null||!selected(display)) { outputs.remove(id).dismiss(); }
        }
        for(Display display:manager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)) {
            if(!selected(display))continue;
            Output output=outputs.get(display.getDisplayId());
            if(output==null) {
                output=new Output(display);
                try { output.show();outputs.put(display.getDisplayId(),output); }
                catch(android.view.WindowManager.InvalidDisplayException error) {Log.w("vkrender","Display removed during attach",error);}
            } else { output.applySize();output.publish(); }
        }
        updateSnapshot();
    }
    boolean ready(){return started;}
    boolean eligible(int id) {
        Display display=manager.getDisplay(id);
        return display!=null&&id!=activity.getWindowManager().getDefaultDisplay().getDisplayId()
               &&(display.getFlags()&Display.FLAG_PRESENTATION)!=0;
    }
    void setOutput(int id,boolean enabledValue,float x,float y,float width,float height) {
        if(!started||!eligible(id))return;
        if(all){enabled.addAll(outputs.keySet());all=false;}
        if(enabledValue){enabled.add(id);crops.put(id,new float[]{x,y,width,height});}else enabled.remove(id);
        refresh();
    }
    String status(){return "{\"ready\":"+started+",\"displays\":"+snapshot+",\"native\":"+nativeDisplayStatus()+"}";}
    private void updateSnapshot() {
        org.json.JSONArray array=new org.json.JSONArray();
        for(Display display:manager.getDisplays())try {
            int id=display.getDisplayId();Output output=outputs.get(id);
            org.json.JSONObject item=new org.json.JSONObject();item.put("id",id);item.put("name",display.getName());
            item.put("eligible",eligible(id));item.put("selected",selected(display));
            item.put("surfaceReady",output!=null&&output.view!=null&&output.view.getHolder().getSurface().isValid());
            org.json.JSONArray crop=new org.json.JSONArray();for(float v:crops.getOrDefault(id,new float[]{0,0,1,1}))crop.put(v);
            item.put("crop",crop);array.put(item);
        }catch(org.json.JSONException ignored){}
        snapshot=array.toString();
    }
    public void onDisplayAdded(int id){refresh();}
    public void onDisplayChanged(int id){refresh();}
    public void onDisplayRemoved(int id){refresh();}
    private final class Output extends Presentation implements SurfaceHolder.Callback {
        final int id;SurfaceView view;
        Output(Display display){super(activity,display);id=display.getDisplayId();}
        protected void onCreate(Bundle state) {
            super.onCreate(state);view=new SurfaceView(getContext());setContentView(view);
            view.getHolder().addCallback(this);applySize();
        }
        void applySize() {
            if(view==null)return;
            int[] size=sizes.get(id);
            if(size==null)view.getHolder().setSizeFromLayout();else view.getHolder().setFixedSize(size[0],size[1]);
        }
        void publish(){publish(false);}
        void publish(boolean recreate) {
            if(view==null||!view.getHolder().getSurface().isValid())return;
            float[] c=crops.getOrDefault(id,new float[]{0,0,1,1});
            nativeSetDisplaySurface(id,view.getHolder().getSurface(),c[0],c[1],c[2],c[3],recreate);updateSnapshot();
        }
        public void surfaceCreated(SurfaceHolder holder){publish();}
        public void surfaceChanged(SurfaceHolder holder,int format,int width,int height){publish(true);}
        public void surfaceDestroyed(SurfaceHolder holder){nativeSetDisplaySurface(id,null,0,0,1,1,false);updateSnapshot();}
        public void dismiss(){nativeSetDisplaySurface(id,null,0,0,1,1,false);super.dismiss();}
    }
}
