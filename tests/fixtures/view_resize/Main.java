// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (android.app.Activity,
// android.view.View, android.content.res.Configuration). No GPL-3.0 sources.
//
// A View-tree app whose window is resized. It fills its window with one color and draws a
// square of another in each corner, so a screenshot shows whether the picture is the
// window's size. What the app is told is logged as `VR: ...`: its view's size, and the
// configuration change in the activity, in a view and in the application's registered
// callbacks, each with what the resources then say the screen is. It exits 2.5 s after the
// first configuration change, or after 12 s without one.
package com.acl.viewresize;

import android.app.Activity;
import android.content.ComponentCallbacks;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.Bundle;
import android.os.Handler;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.View;
import android.widget.FrameLayout;

public class Main extends Activity {
    static final String TAG = "acl";
    static final int FILL = 0xFF20A040, CORNER = 0xFFE02020, SQUARE = 40;
    private int configs, sizes, draws;
    private boolean created2;
    private Handler handler;

    final class Corners extends View {
        private final Paint paint = new Paint();
        Corners(Context c) { super(c); }
        @Override protected void onSizeChanged(int w, int h, int ow, int oh) {
            sizes++;
            Log.i(TAG, "VR: size " + w + "x" + h);
        }
        @Override protected void onConfigurationChanged(Configuration c) {
            Log.i(TAG, "VR: view onConfigurationChanged " + c.screenWidthDp + "x" + c.screenHeightDp);
        }
        @Override protected void onDraw(Canvas canvas) {
            draws++;
            final int w = getWidth(), h = getHeight();
            canvas.drawColor(FILL);
            paint.setColor(CORNER);
            canvas.drawRect(0, 0, SQUARE, SQUARE, paint);
            canvas.drawRect(w - SQUARE, 0, w, SQUARE, paint);
            canvas.drawRect(0, h - SQUARE, SQUARE, h, paint);
            canvas.drawRect(w - SQUARE, h - SQUARE, w, h, paint);
        }
    }

    private void finishRun(String why) {
        Log.i(TAG, "VR: done (" + why + ") configs " + configs + " sizes " + sizes + " draws " + draws
                + " created " + (created2 ? 2 : 1));
        System.exit(0);
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        if (handler != null) created2 = true;  // a4l does not relaunch; this would show it had
        handler = new Handler(getMainLooper());
        final FrameLayout root = new FrameLayout(this);
        root.addView(new Corners(this), new FrameLayout.LayoutParams(-1, -1));
        setContentView(root);
        getApplication().registerComponentCallbacks(new ComponentCallbacks() {
            public void onConfigurationChanged(Configuration c) {
                Log.i(TAG, "VR: application onConfigurationChanged " + c.screenWidthDp + "x"
                        + c.screenHeightDp);
            }
            public void onLowMemory() {}
        });
        final DisplayMetrics dm = getResources().getDisplayMetrics();
        final Configuration rc = getResources().getConfiguration();
        Log.i(TAG, "VR: created resources " + rc.screenWidthDp + "x" + rc.screenHeightDp + " metrics "
                + dm.widthPixels + "x" + dm.heightPixels);
        try {  // the fixture's manifest states 42 / "1.2.3"
            android.content.pm.PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
            Log.i(TAG, "VR: package version " + pi.versionCode + " / " + pi.versionName);
        } catch (Exception e) {
            Log.i(TAG, "VR: package version unknown: " + e);
        }
        handler.postDelayed(new Runnable() {
            public void run() { finishRun("no configuration change"); }
        }, 12000);
    }

    @Override
    public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        final DisplayMetrics dm = getResources().getDisplayMetrics();
        final Configuration rc = getResources().getConfiguration();
        Log.i(TAG, "VR: activity onConfigurationChanged " + c.screenWidthDp + "x" + c.screenHeightDp
                + " resources " + rc.screenWidthDp + "x" + rc.screenHeightDp + " metrics "
                + dm.widthPixels + "x" + dm.heightPixels + " orientation " + c.orientation
                // The largest software layer a View may have follows the display: 4 bytes a pixel.
                + " layer limit "
                + android.view.ViewConfiguration.get(this).getScaledMaximumDrawingCacheSize());
        if (configs++ == 0) {
            handler.postDelayed(new Runnable() {
                public void run() { finishRun("after the configuration change"); }
            }, 2500);
        }
    }
}
