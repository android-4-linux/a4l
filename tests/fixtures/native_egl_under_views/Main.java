// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (SurfaceView, SurfaceHolder,
// System.loadLibrary). No GPL-3.0 sources.
//
// A screen drawn by an engine through native EGL, opened from an app's own views: what a
// Flutter screen in an app that is otherwise built of views is. The whole window is not the
// engine's, as it is for an app whose first Activity is a Flutter one.
//
// Main, the launcher, shows a plain view and starts Screen. Screen lays an EngineView (a
// SurfaceView subclass) over the whole window and a red banner view over its top, and hands
// the view's Surface to the library (native_egl.c), which draws green and blue halves with a
// light band along the bottom from a thread of its own. The test reads the colors off a
// screenshot.
package com.acl.nativeegl;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.FrameLayout;

public class Main extends Activity {
    static final int BANNER_HEIGHT = 96;

    static {
        System.loadLibrary("nativeegl");
    }

    static native boolean start(Surface surface, int width, int height);
    static native void stop();

    static void log(String s) {
        Log.i("acl", "NATIVEEGL: " + s);
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        final View plain = new View(this);
        plain.setBackgroundColor(0xFF404040);
        setContentView(plain);
        plain.postDelayed(new Runnable() {
            @Override public void run() {
                startActivity(new Intent(Main.this, Screen.class));
            }
        }, 300);
    }

    public static class Screen extends Activity {
        @Override
        protected void onCreate(Bundle b) {
            super.onCreate(b);
            final FrameLayout root = new FrameLayout(this);
            root.addView(new EngineView(this), new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT));
            final View banner = new View(this);
            banner.setBackgroundColor(0xFFC02020);
            root.addView(banner, new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, BANNER_HEIGHT, Gravity.TOP));
            setContentView(root);
            log("screen created");
        }
    }

    /** What an engine's view is: a SurfaceView subclass whose Surface goes to native code. */
    static class EngineView extends SurfaceView implements SurfaceHolder.Callback {
        private boolean mStarted;

        EngineView(Activity a) {
            super(a);
            getHolder().addCallback(this);
        }

        @Override public void surfaceCreated(SurfaceHolder holder) {
            log("surfaceCreated");
        }

        @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
            log("surfaceChanged " + w + "x" + h);
            if (!mStarted) {
                mStarted = start(holder.getSurface(), w, h);
                log("engine started=" + mStarted);
            }
        }

        @Override public void surfaceDestroyed(SurfaceHolder holder) {
            log("surfaceDestroyed");
            stop();
            mStarted = false;
        }
    }
}
