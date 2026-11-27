// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (SurfaceView, SurfaceHolder,
// javax.microedition.khronos.egl.EGL10, GLES20). No GPL-3.0 sources.
//
// A SurfaceView that an engine draws GL into, under the app's other views, in the app's
// second Activity: what a map app is (its map under its search box), and not what a4l's two
// SurfaceView cases were (a game whose whole window is the GL surface, a player's video).
//
// Main, the launcher, shows a plain view and starts Map. Map lays a MapView (a SurfaceView
// subclass) over the whole window and a red banner view over its top. MapView draws on a
// thread of its own through EGL10: green on the left half, blue on the right, and a light
// band along the bottom of GL's coordinates, which must be the bottom of the screen. The
// test reads the colors off a screenshot.
package com.acl.glplane;

import android.app.Activity;
import android.content.Intent;
import android.opengl.GLES20;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.FrameLayout;

import javax.microedition.khronos.egl.EGL10;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.egl.EGLContext;
import javax.microedition.khronos.egl.EGLDisplay;
import javax.microedition.khronos.egl.EGLSurface;

public class Main extends Activity {
    static final String TAG = "acl";
    static final int BANNER_HEIGHT = 96;

    static void log(String s) {
        Log.i(TAG, "GLPLANE: " + s);
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        final View plain = new View(this);
        plain.setBackgroundColor(0xFF404040);
        setContentView(plain);
        plain.postDelayed(new Runnable() {
            @Override public void run() {
                startActivity(new Intent(Main.this, Map.class));
            }
        }, 300);
    }

    public static class Map extends Activity {
        @Override
        protected void onCreate(Bundle b) {
            super.onCreate(b);
            final FrameLayout root = new FrameLayout(this);
            root.addView(new MapView(this), new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
            final View banner = new View(this);
            banner.setBackgroundColor(0xFFC02020);
            root.addView(banner, new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, BANNER_HEIGHT, Gravity.TOP));
            setContentView(root);
            log("map activity created");
        }
    }

    /** What an engine's view is: a SurfaceView subclass that draws GL when it has a surface. */
    static class MapView extends SurfaceView implements SurfaceHolder.Callback, Runnable {
        private volatile boolean mRunning;
        private volatile int mWidth, mHeight;
        private Thread mThread;

        MapView(Activity a) {
            super(a);
            getHolder().addCallback(this);
        }

        @Override public void surfaceCreated(SurfaceHolder holder) {
            log("surfaceCreated");
        }

        @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
            log("surfaceChanged " + w + "x" + h);
            mWidth = w;
            mHeight = h;
            if (mThread == null) {
                mRunning = true;
                mThread = new Thread(this, "map-render");
                mThread.start();
            }
        }

        @Override public void surfaceDestroyed(SurfaceHolder holder) {
            log("surfaceDestroyed");
            mRunning = false;
        }

        private static String fail(EGL10 egl, String step) {
            return "FAILED at " + step + " (error 0x" + Integer.toHexString(egl.eglGetError())
                    + ")";
        }

        @Override public void run() {
            final EGL10 egl = (EGL10) EGLContext.getEGL();
            final EGLDisplay d = egl.eglGetDisplay(EGL10.EGL_DEFAULT_DISPLAY);
            if (d == EGL10.EGL_NO_DISPLAY) { log(fail(egl, "eglGetDisplay")); return; }
            if (!egl.eglInitialize(d, new int[2])) { log(fail(egl, "eglInitialize")); return; }
            final int[] attrs = {EGL10.EGL_RENDERABLE_TYPE, 4 /* EGL_OPENGL_ES2_BIT */,
                    EGL10.EGL_RED_SIZE, 8, EGL10.EGL_GREEN_SIZE, 8, EGL10.EGL_BLUE_SIZE, 8,
                    EGL10.EGL_NONE};
            final EGLConfig[] configs = new EGLConfig[1];
            final int[] n = new int[1];
            if (!egl.eglChooseConfig(d, attrs, configs, 1, n) || n[0] < 1) {
                log(fail(egl, "eglChooseConfig"));
                return;
            }
            final EGLContext c = egl.eglCreateContext(d, configs[0], EGL10.EGL_NO_CONTEXT,
                    new int[] {0x3098 /* EGL_CONTEXT_CLIENT_VERSION */, 2, EGL10.EGL_NONE});
            if (c == null || c == EGL10.EGL_NO_CONTEXT) {
                log(fail(egl, "eglCreateContext"));
                return;
            }
            final EGLSurface s = egl.eglCreateWindowSurface(d, configs[0], getHolder(), null);
            if (s == null || s == EGL10.EGL_NO_SURFACE) {
                log(fail(egl, "eglCreateWindowSurface"));
                return;
            }
            if (!egl.eglMakeCurrent(d, s, s, c)) { log(fail(egl, "eglMakeCurrent")); return; }
            log("window surface made");
            final long start = SystemClock.elapsedRealtime();
            int frames = 0;
            while (mRunning) {
                final int w = mWidth, h = mHeight;
                GLES20.glViewport(0, 0, w, h);
                GLES20.glEnable(GLES20.GL_SCISSOR_TEST);
                GLES20.glScissor(0, 0, w / 2, h);
                GLES20.glClearColor(0x20 / 255f, 0xC0 / 255f, 0x20 / 255f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                GLES20.glScissor(w / 2, 0, w - w / 2, h);
                GLES20.glClearColor(0x20 / 255f, 0x20 / 255f, 0xC0 / 255f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                GLES20.glScissor(0, 0, w, h / 4);  // GL's y runs up: this is the bottom
                GLES20.glClearColor(0xE0 / 255f, 0xE0 / 255f, 0xE0 / 255f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                if (!egl.eglSwapBuffers(d, s)) {
                    log(fail(egl, "eglSwapBuffers"));
                    return;
                }
                if (++frames == 120) {
                    // A window's swap waits for the display: 120 of them are about two
                    // seconds at 60 Hz, and no time at all if nothing holds the thread.
                    log("120 frames in " + (SystemClock.elapsedRealtime() - start) + " ms");
                }
            }
            egl.eglMakeCurrent(d, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_SURFACE,
                    EGL10.EGL_NO_CONTEXT);
            egl.eglDestroySurface(d, s);
            egl.eglDestroyContext(d, c);
            log("render thread ended after " + frames + " frames");
        }
    }
}
