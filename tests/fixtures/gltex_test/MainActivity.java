// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl test fixture; Android SDK TextureView / EGL10 / GLES20 docs. No GPL-3.0 sources.
//
// Minimum-viable repro of the SurfaceTexture -> present path (the TextureView GL producer acl
// bridges for mixed Java-EGL/native-GL apps). A full-screen TextureView; on SurfaceTextureAvailable
// a render thread sets up EGL10 on the SurfaceTexture (eglCreateWindowSurface -> acl's producer),
// then loops GLES20 rendering an animated full-screen color cycle with a moving white box (via
// glScissor+glClear, so no shaders) and eglSwapBuffers. Exercises: eglCreateWindowSurfaceTexture,
// the eglMakeCurrent/GLES20/eglSwapBuffers donor-%fs routing, the gbm readback, the SurfaceTexture
// feed, and the TextureLayer composite. Unlike Telegram's tiny 200x150 logo, this fills the screen
// and animates, so the path is unambiguously visible.
package com.acl.gltex;

import android.app.Activity;
import android.graphics.SurfaceTexture;
import android.opengl.GLES20;
import android.os.Bundle;
import android.util.Log;
import android.view.TextureView;

import javax.microedition.khronos.egl.EGL10;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.egl.EGLContext;
import javax.microedition.khronos.egl.EGLDisplay;
import javax.microedition.khronos.egl.EGLSurface;

public class MainActivity extends Activity implements TextureView.SurfaceTextureListener {
    static final String TAG = "acl.gltex";
    RenderThread thread;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        TextureView tv = new TextureView(this);
        tv.setSurfaceTextureListener(this);
        setContentView(tv);
        Log.i(TAG, "onCreate: full-screen TextureView content set");
    }

    @Override
    public void onSurfaceTextureAvailable(SurfaceTexture st, int w, int h) {
        Log.i(TAG, "onSurfaceTextureAvailable " + w + "x" + h);
        thread = new RenderThread(st, w, h);
        thread.start();
    }

    @Override
    public void onSurfaceTextureSizeChanged(SurfaceTexture st, int w, int h) {
        Log.i(TAG, "onSurfaceTextureSizeChanged " + w + "x" + h);
        if (thread != null) { thread.w = w; thread.h = h; }
    }

    @Override
    public boolean onSurfaceTextureDestroyed(SurfaceTexture st) {
        if (thread != null) { thread.running = false; thread = null; }
        return true;
    }

    @Override
    public void onSurfaceTextureUpdated(SurfaceTexture st) {}

    static class RenderThread extends Thread {
        final SurfaceTexture st;
        volatile int w, h;
        volatile boolean running = true;

        RenderThread(SurfaceTexture st, int w, int h) { this.st = st; this.w = w; this.h = h; }

        @Override
        public void run() {
            final int EGL_OPENGL_ES2_BIT = 0x0004;
            final int EGL_CONTEXT_CLIENT_VERSION = 0x3098;
            EGL10 egl = (EGL10) EGLContext.getEGL();
            EGLDisplay dpy = egl.eglGetDisplay(EGL10.EGL_DEFAULT_DISPLAY);
            egl.eglInitialize(dpy, new int[2]);
            int[] cfgAttr = {
                EGL10.EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL10.EGL_RED_SIZE, 8, EGL10.EGL_GREEN_SIZE, 8, EGL10.EGL_BLUE_SIZE, 8,
                EGL10.EGL_ALPHA_SIZE, 8, EGL10.EGL_NONE,
            };
            EGLConfig[] cfgs = new EGLConfig[1];
            int[] num = new int[1];
            if (!egl.eglChooseConfig(dpy, cfgAttr, cfgs, 1, num) || num[0] == 0) {
                Log.e(TAG, "eglChooseConfig failed");
                return;
            }
            int[] ctxAttr = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL10.EGL_NONE };
            EGLContext ctx = egl.eglCreateContext(dpy, cfgs[0], EGL10.EGL_NO_CONTEXT, ctxAttr);
            EGLSurface sfc = egl.eglCreateWindowSurface(dpy, cfgs[0], st, null);
            if (sfc == null || sfc == EGL10.EGL_NO_SURFACE) {
                Log.e(TAG, "eglCreateWindowSurface(SurfaceTexture) failed");
                return;
            }
            egl.eglMakeCurrent(dpy, sfc, sfc, ctx);
            Log.i(TAG, "GL context ready — rendering animated pattern (" + GLES20.glGetString(GLES20.GL_RENDERER) + ")");

            long t0 = System.currentTimeMillis();
            int frame = 0;
            while (running) {
                float t = (System.currentTimeMillis() - t0) / 1000f;
                float r = (float) (0.5 + 0.5 * Math.sin(t));
                float g = (float) (0.5 + 0.5 * Math.sin(t + 2.094));
                float bl = (float) (0.5 + 0.5 * Math.sin(t + 4.188));
                GLES20.glClearColor(r, g, bl, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                // A white box that slides left<->right — proves per-frame GL draw (no shaders).
                int box = Math.max(40, Math.min(w, h) / 8);
                int bx = (int) ((Math.sin(t) * 0.45 + 0.5) * Math.max(1, w - box));
                GLES20.glEnable(GLES20.GL_SCISSOR_TEST);
                GLES20.glScissor(bx, h / 2 - box / 2, box, box);
                GLES20.glClearColor(1f, 1f, 1f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                GLES20.glDisable(GLES20.GL_SCISSOR_TEST);
                egl.eglSwapBuffers(dpy, sfc);
                if (frame++ % 60 == 0) Log.i(TAG, "rendered frame " + frame);
                try { Thread.sleep(16); } catch (InterruptedException e) { /* keep going */ }
            }
        }
    }
}
