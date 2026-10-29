package dev.vkrender.renderer;

import android.app.Service;
import android.content.*;
import android.graphics.Canvas;
import android.graphics.Color;
import android.os.*;
import android.util.Log;
import android.view.Surface;
import org.json.*;
import dev.vkrender.IVkRendererService;

/** Debug-only real Binder control test in a separate producer process. */
public final class ControlTestService extends Service {
    private static final String TAG="RendererControlTest";
    private boolean bound;
    private final ServiceConnection connection=new ServiceConnection(){
        public void onServiceConnected(ComponentName name,IBinder binder){
            new Thread(()->runTest(IVkRendererService.Stub.asInterface(binder)),"control-test").start();
        }
        public void onServiceDisconnected(ComponentName name){Log.e(TAG,"FAIL renderer disconnected");}
    };
    public IBinder onBind(Intent intent){return null;}
    public int onStartCommand(Intent intent,int flags,int id){
        if(!bound)bound=bindService(new Intent(this,RendererService.class),connection,BIND_AUTO_CREATE);
        return START_NOT_STICKY;
    }
    private static void check(boolean condition,String message){if(!condition)throw new AssertionError(message);}
    private static JSONObject status(IVkRendererService api)throws Exception{return new JSONObject(api.getControlStatus());}
    private static JSONObject slot(IVkRendererService api,int wanted)throws Exception{
        JSONArray streams=status(api).getJSONObject("state").optJSONArray("streams");
        if(streams!=null)for(int i=0;i<streams.length();i++){
            JSONObject item=streams.getJSONObject(i);
            if((wanted<0 && item.getInt("slot")>=12) || item.getInt("slot")==wanted)return item;
        }
        return null;
    }
    private interface Condition{boolean ready()throws Exception;}
    private static void waitFor(Condition condition,String message)throws Exception{
        long end=SystemClock.elapsedRealtime()+6000;
        while(SystemClock.elapsedRealtime()<end){if(condition.ready())return;Thread.sleep(30);}
        throw new AssertionError(message);
    }
    private static void draw(Surface surface,int count)throws Exception{
        for(int i=0;i<count;i++){
            Canvas canvas=surface.lockHardwareCanvas();canvas.drawColor((i&1)==0?Color.RED:Color.GREEN);
            surface.unlockCanvasAndPost(canvas);Thread.sleep(16);
        }
    }
    private void runTest(IVkRendererService api){
        Surface surface=null;Binder owner=new Binder();
        try{
            check(api.getControlVersion()==1,"version");
            JSONObject caps=new JSONObject(api.getControlCapabilities());
            check(caps.getBoolean("moderation") && !caps.getBoolean("displays"),"capabilities");
            check(api.submitControl("layout",-1,"wrong")==1,"invalid layout");
            check(api.submitControl("rotate_seconds",-1,"nan")==1,"invalid duration");
            check(api.submitControl("camera",-1,"1")==2,"unsupported camera");
            waitFor(()->status(api).getBoolean("ready"),"renderer ready");
            check(caps.getBoolean("preview"),"preview capability");
            check(status(api).getInt("previewCaptures")==0,"no readback without request");
            surface=api.acquireInputSurfaceWithOwner(0,owner);check(surface!=null,"surface");draw(surface,30);
            waitFor(()->slot(api,-1)!=null,"roster");int id=slot(api,-1).getInt("slot");
            check(api.submitControl("layout",-1,"wall")==0,"layout queue");
            check(api.submitControl("max_visible",-1,"2")==0,"config queue");
            check(api.submitControl("pin",id,"")==0,"pin queue");
            check(api.submitControl("hide",id,"")==0,"hide queue");
            waitFor(()->slot(api,id).getBoolean("hidden") && !slot(api,id).getBoolean("visible"),"hidden state");
            draw(surface,120);Log.i(TAG,"PASS hidden producer posted120frames without stall");
            check(api.submitControl("show",id,"")==0,"show queue");
            check(api.submitControl("spotlight",id,"")==0,"spotlight queue");
            waitFor(()->slot(api,id).getBoolean("visible"),"shown state");
            draw(surface,60); // allow the entry animation to finish before freezing scene motion
            check(api.submitControl("pause",-1,"1")==0,"pause queue");
            waitFor(()->status(api).getJSONObject("state").getBoolean("paused"),"pause applied");
            waitFor(()->api.getPreviewJpeg()!=null,"preview arrives asynchronously");
            byte[] jpeg=api.getPreviewJpeg();check(jpeg.length<=128*1024,"bounded Binder JPEG");
            android.graphics.Bitmap preview=android.graphics.BitmapFactory.decodeByteArray(jpeg,0,jpeg.length);
            check(preview!=null && preview.getWidth()<=320 && preview.getHeight()<=180,"preview dimensions");
            int min=255,max=0;for(int y=0;y<preview.getHeight();y++)for(int x=0;x<preview.getWidth();x++){int c=preview.getPixel(x,y);int level=(Color.red(c)+Color.green(c)+Color.blue(c))/3;min=Math.min(min,level);max=Math.max(max,level);}
            Log.i(TAG,"preview luminance min="+min+" max="+max+" center="+Integer.toHexString(preview.getPixel(preview.getWidth()/2,preview.getHeight()/2)));
            try(java.io.FileOutputStream out=openFileOutput("preview.jpg",MODE_PRIVATE)){out.write(jpeg);}
            check(max-min>20,"preview contains rendered scene");preview.recycle();
            Thread.sleep(400);int captures=status(api).getInt("previewCaptures");Thread.sleep(400);
            check(status(api).getInt("previewCaptures")==captures,"readback stops when requests stop");
            Log.i(TAG,"PASS asynchronous bounded JPEG preview and no idle readback");
            startActivity(new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            waitFor(()->!status(api).getBoolean("ready"),"Activity paused ready false");
            long start=SystemClock.elapsedRealtime();
            check(api.submitControl("pause",-1,"0")==3,"paused unavailable");
            check(api.getPreviewJpeg()==null,"paused preview unavailable");
            check(SystemClock.elapsedRealtime()-start<500,"unavailable bounded");
            startActivity(new Intent(this,MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK|Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));
            waitFor(()->status(api).getBoolean("ready"),"Activity resumed");
            check(api.submitControl("pause",-1,"0")==0,"unpause queue");
            check(api.submitControl("unspotlight",-1,"")==0,"unspotlight queue");
            check(api.submitControl("kick",id,"")==0,"kick queue");
            waitFor(()->slot(api,id)==null,"kick removed roster");
            Log.i(TAG,"PASS control version/capabilities/status, validation, queued actions, hidden progress, pause/resume bounds and kick");
        }catch(Throwable error){Log.e(TAG,"FAIL",error);}
        finally{
            try{api.releaseInputSurfaceWithOwner(0,owner);}catch(Exception ignored){}
            if(surface!=null)surface.release();stopSelf();
        }
    }
    public void onDestroy(){if(bound)unbindService(connection);super.onDestroy();}
}
