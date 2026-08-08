package dev.vkrender.wall;

import android.app.Activity;
import android.os.Bundle;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;

/**
 * M30: the spanning wall on Android - three SurfaceViews in ONE activity, all
 * driven by one VkDevice, arranged over a shared virtual canvas.
 *
 * <p>This is the Android counterpart to what M29 proved on Windows. The three
 * panels sit on a single display here, which is the stepping stone to three
 * real displays: the canvas maths is identical and only the origin of the
 * surfaces changes (SurfaceView today, Presentation per display later).
 *
 * <p>Each panel reports its on-screen position and size to native, so the
 * canvas is the union of those rects and bezel compensation follows exactly
 * the desktop model - the rect is the panel's physical footprint and the bezel
 * is how much of its own edge is hidden. See docs/MULTIDISPLAY.md.
 *
 * <p>The Add/Remove buttons drive simulated presenters so layout reflow (and
 * next, the entry/exit animation) can be exercised without a decoder, a
 * network sender or a camera in the loop.
 */
public class WallActivity extends Activity {
    static {
        System.loadLibrary("vkrender_wall");
    }

    private native boolean nativeInit(Object assetManager);
    private native void nativeSetPanel(int index, Surface surface, int x, int y, int w, int h,
                                       int bezel);
    private native boolean nativeStart();
    private native void nativeStop();
    private native int nativeAddPresenter();
    private native int nativeRemovePresenter();

    private static final int PANELS = 3;
    private final SurfaceView[] views = new SurfaceView[PANELS];
    private final boolean[] ready = new boolean[PANELS];
    private TextView status;
    private int bezel = 12;      // simulated frame width, tunable from the UI
    private boolean started = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_wall);

        status = findViewById(R.id.status);
        views[0] = findViewById(R.id.panel0);
        views[1] = findViewById(R.id.panel1);
        views[2] = findViewById(R.id.panel2);

        if (!nativeInit(getAssets())) {
            status.setText("Vulkan init failed - see logcat");
            return;
        }

        for (int i = 0; i < PANELS; i++) {
            final int index = i;
            views[i].getHolder().addCallback(new SurfaceHolder.Callback() {
                @Override
                public void surfaceCreated(SurfaceHolder holder) {}

                @Override
                public void surfaceChanged(SurfaceHolder holder, int fmt, int w, int h) {
                    // Location on screen is what defines this panel's slice of
                    // the canvas, so it is reported with the surface rather
                    // than assumed from the layout file.
                    int[] loc = new int[2];
                    views[index].getLocationOnScreen(loc);
                    nativeSetPanel(index, holder.getSurface(), loc[0], loc[1], w, h, bezel);
                    ready[index] = true;
                    maybeStart();
                }

                @Override
                public void surfaceDestroyed(SurfaceHolder holder) {
                    ready[index] = false;
                    nativeSetPanel(index, null, 0, 0, 0, 0, bezel);
                }
            });
        }

        Button add = findViewById(R.id.add);
        Button remove = findViewById(R.id.remove);
        Button bezelUp = findViewById(R.id.bezel_up);
        Button bezelDown = findViewById(R.id.bezel_down);

        add.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                setStatus(nativeAddPresenter());
            }
        });
        remove.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                setStatus(nativeRemovePresenter());
            }
        });
        bezelUp.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                bezel += 6;
                reportPanels();
            }
        });
        bezelDown.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                bezel = Math.max(0, bezel - 6);
                reportPanels();
            }
        });
        setStatus(0);
    }

    /** Re-report every live panel, e.g. after the bezel changed. */
    private void reportPanels() {
        for (int i = 0; i < PANELS; i++) {
            if (!ready[i]) continue;
            int[] loc = new int[2];
            views[i].getLocationOnScreen(loc);
            nativeSetPanel(i, views[i].getHolder().getSurface(), loc[0], loc[1],
                    views[i].getWidth(), views[i].getHeight(), bezel);
        }
        setStatusText();
    }

    private void maybeStart() {
        if (started) return;
        for (int i = 0; i < PANELS; i++)
            if (!ready[i]) return;   // wait for the whole wall
        started = nativeStart();
        setStatusText();
    }

    private int presenters = 0;

    private void setStatus(int n) {
        presenters = n;
        setStatusText();
    }

    private void setStatusText() {
        status.setText("presenters " + presenters + "   bezel " + bezel + "px"
                + (started ? "" : "   (waiting for surfaces)"));
    }

    @Override
    protected void onDestroy() {
        nativeStop();
        super.onDestroy();
    }
}
