package dev.vkrender.renderer;

import android.app.Service;
import android.content.*;
import android.graphics.Canvas;
import android.graphics.Color;
import android.os.*;
import android.util.Log;
import android.view.Surface;
import dev.vkrender.IVkRendererService;

/** Debug APK only; start with am startservice while renderer Activity is visible.
 * Uses a genuine separate process/token so killProcess exercises kernel Binder death.
 */
public final class SurfaceLifecycleTestService extends Service {
    private static final String TAG="SurfaceLifecycleTest";
    private boolean bound;
    private boolean legacy, rapid;
    private final ServiceConnection connection=new ServiceConnection() {
        public void onServiceConnected(ComponentName name,IBinder binder) {
            new Thread(() -> runTest(IVkRendererService.Stub.asInterface(binder)),"surface-test").start();
        }
        public void onServiceDisconnected(ComponentName name) { Log.e(TAG,"renderer disconnected"); }
    };
    public IBinder onBind(Intent intent) { return null; }
    public int onStartCommand(Intent intent,int flags,int id) {
        rapid=intent!=null&&intent.getBooleanExtra("rapid",false);
        legacy=intent!=null&&intent.getBooleanExtra("legacy",false);
        if(!bound)bound=bindService(new Intent(this,RendererService.class),connection,BIND_AUTO_CREATE);
        return START_NOT_STICKY;
    }
    private void draw(Surface surface,int color)throws Exception {
        if(surface==null)throw new IllegalStateException("null acquired surface");
        for(int i=0;i<(rapid?3:60);i++) {
            Canvas canvas=surface.lockHardwareCanvas();
            canvas.drawColor(color);
            android.graphics.Paint paint=new android.graphics.Paint();
            paint.setColor(Color.WHITE);paint.setTextSize(80);
            canvas.drawText("Binder frame "+i,60,120,paint);
            surface.unlockCanvasAndPost(canvas);
            Thread.sleep(20);
        }
    }
    private void runTest(IVkRendererService api) {
        try {
            if(rapid) {
                Binder prior=null;
                for(int i=0;i<20;i++) {
                    Binder owner=new Binder();
                    Surface surface=api.acquireInputSurfaceWithOwner(0,owner);
                    if(prior!=null)api.releaseInputSurfaceWithOwner(0,prior);
                    draw(surface,(i%2==0)?Color.RED:Color.GREEN);
                    api.releaseInputSurfaceWithOwner(0,owner);surface.release();prior=owner;
                }
                Thread.sleep(1000);
                Log.i(TAG,"PASS 20 rapid release/reacquire generations");stopSelf();return;
            }
            if(legacy) {
                Surface first=api.acquireInputSurface();draw(first,Color.YELLOW);
                api.releaseInputSurface();first.release();
                Surface second=api.acquireInputSurface();draw(second,Color.CYAN);
                api.releaseInputSurface();second.release();
                Log.i(TAG,"PASS legacy release/reacquire");stopSelf();return;
            }
            Binder a=new Binder(),b=new Binder(),c=new Binder();
            Surface first=api.acquireInputSurfaceWithOwner(0,a);draw(first,Color.RED);
            api.releaseInputSurfaceWithOwner(0,a);first.release();
            Surface second=api.acquireInputSurfaceWithOwner(0,b);draw(second,Color.GREEN);
            api.releaseInputSurfaceWithOwner(0,a); // stale release must leave B active
            draw(second,Color.BLUE);
            api.releaseInputSurfaceWithOwner(0,b);second.release();
            Log.i(TAG,"PASS tracked release/reacquire and stale release; waiting last-stream exit");
            Thread.sleep(2500);
            Surface third=api.acquireInputSurfaceWithOwner(0,c);draw(third,Color.MAGENTA);
            Log.i(TAG,"PASS drawing before intentional producer process death");
            android.os.Process.killProcess(android.os.Process.myPid());
        } catch(Throwable error) { Log.e(TAG,"FAIL",error);stopSelf(); }
    }
    public void onDestroy() { if(bound)unbindService(connection);super.onDestroy(); }
}
