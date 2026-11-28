// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (SurfaceView.setZOrderOnTop,
// SurfaceHolder.setFormat, R.attr.windowIsTranslucent,
// javax.microedition.khronos.egl.EGL10, GLES20). No GPL-3.0 sources.
//
// A screen an engine draws with a clear background, over the screen before it: what a
// Flutter screen in a translucent Activity is. The engine's SurfaceView asks to be over its
// window (setZOrderOnTop) and for a format with alpha, and clears its surface to nothing
// where it draws nothing. On Android the screen underneath shows there.
//
// Main, the launcher, shows a plain view and starts Under: an app whose first Activity has
// the GL view is given the whole window as its GL surface instead. Under is a red banner
// over blue, and starts Card when it is resumed. Card's theme is
// Theme.Translucent.NoTitleBar and its only view is a CardView, which draws through EGL10
// on a thread of its own: nothing, but for a yellow rectangle in the middle and a strip of
// half black down the right edge. The test reads the colors off a screenshot. With
// A4L_TEST_OPAQUE=1 the view asks for no format with alpha, and what it leaves clear is
// black, as on Android.
package com.acl.ontop;

import android.app.Activity;
import android.content.Intent;
import android.graphics.PixelFormat;
import android.opengl.GLES20;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
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
    static final int BANNER_HEIGHT = 96;
    static final int CARD_WIDTH = 400, CARD_HEIGHT = 200, STRIP_WIDTH = 100;

    static void log(String s) {
        Log.i("acl", "ONTOP: " + s);
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        final View plain = new View(this);
        plain.setBackgroundColor(0xFF404040);
        setContentView(plain);
        plain.postDelayed(new Runnable() {
            @Override public void run() {
                startActivity(new Intent(Main.this, Under.class));
            }
        }, 300);
    }

    public static class Under extends Activity {
        @Override
        protected void onCreate(Bundle b) {
            super.onCreate(b);
            final FrameLayout root = new FrameLayout(this);
            root.setBackgroundColor(0xFF2020C0);
            final View banner = new View(this);
            banner.setBackgroundColor(0xFFC02020);
            root.addView(banner, new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, BANNER_HEIGHT, Gravity.TOP));
            setContentView(root);
            // Once this Activity is resumed.
            new Handler(Looper.getMainLooper()).post(new Runnable() {
                @Override public void run() {
                    startActivity(new Intent(Under.this, Card.class));
                }
            });
        }
    }

    public static class Card extends Activity {
        @Override
        protected void onCreate(Bundle b) {
            super.onCreate(b);
            setContentView(new CardView(this));
            log("card created");
        }
    }

    /** An engine's view with a clear background. */
    static final class CardView extends SurfaceView implements SurfaceHolder.Callback, Runnable {
        private volatile boolean mRunning;
        private volatile int mWidth, mHeight;
        private Thread mThread;

        CardView(Activity a) {
            super(a);
            setZOrderOnTop(true);
            // A4L_TEST_OPAQUE: the format is left as it is, which has no alpha. The
            // surface is then opaque, whatever is drawn into its alpha.
            if (!"1".equals(System.getenv("A4L_TEST_OPAQUE"))) {
                getHolder().setFormat(PixelFormat.TRANSLUCENT);
            }
            getHolder().addCallback(this);
        }

        @Override public void surfaceCreated(SurfaceHolder holder) {
        }

        @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
            mWidth = w;
            mHeight = h;
            if (mThread == null) {
                mRunning = true;
                mThread = new Thread(this, "card-render");
                mThread.start();
            }
        }

        @Override public void surfaceDestroyed(SurfaceHolder holder) {
            mRunning = false;
            final Thread t = mThread;
            mThread = null;
            if (t == null) return;
            try {
                t.join(2000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
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
                    EGL10.EGL_ALPHA_SIZE, 8, EGL10.EGL_NONE};
            final EGLConfig[] configs = new EGLConfig[1];
            final int[] n = new int[1];
            if (!egl.eglChooseConfig(d, attrs, configs, 1, n) || n[0] < 1) {
                log(fail(egl, "eglChooseConfig"));
                return;
            }
            final int[] alpha = new int[1], visual = new int[1];
            egl.eglGetConfigAttrib(d, configs[0], EGL10.EGL_ALPHA_SIZE, alpha);
            egl.eglGetConfigAttrib(d, configs[0], EGL10.EGL_NATIVE_VISUAL_ID, visual);
            log("config alpha=" + alpha[0] + " visual=0x" + Integer.toHexString(visual[0]));
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
            int frames = 0;
            while (mRunning) {
                final int w = mWidth, h = mHeight;
                GLES20.glViewport(0, 0, w, h);
                GLES20.glDisable(GLES20.GL_SCISSOR_TEST);
                GLES20.glClearColor(0f, 0f, 0f, 0f);  // nothing
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                GLES20.glEnable(GLES20.GL_SCISSOR_TEST);
                // Half black, in the premultiplied colors a surface with alpha holds.
                GLES20.glScissor(w - STRIP_WIDTH, 0, STRIP_WIDTH, h);
                GLES20.glClearColor(0f, 0f, 0f, 0.5f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                GLES20.glScissor((w - CARD_WIDTH) / 2, (h - CARD_HEIGHT) / 2, CARD_WIDTH,
                        CARD_HEIGHT);
                GLES20.glClearColor(0xE0 / 255f, 0xC0 / 255f, 0x20 / 255f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                if (!egl.eglSwapBuffers(d, s)) {
                    log(fail(egl, "eglSwapBuffers"));
                    return;
                }
                if (++frames == 30) log("30 frames drawn");
            }
            egl.eglMakeCurrent(d, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_SURFACE,
                    EGL10.EGL_NO_CONTEXT);
            egl.eglDestroySurface(d, s);
            egl.eglDestroyContext(d, c);
        }
    }
}
