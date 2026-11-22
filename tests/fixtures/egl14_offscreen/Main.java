// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (android.opengl.EGL14, GLES20). No
// GPL-3.0 sources.
//
// Offscreen GL through android.opengl.EGL14 on the default display, the way a player's
// placeholder surface (media3's PlaceholderSurface) sets it up before it has a view to draw in:
// initialize, choose a pbuffer config, make a context current on a pbuffer, clear it and read the
// pixel back. Logs one line, "EGL14: OK ..." or "EGL14: FAILED at <step> ...", and exits.
package com.acl.egl14;

import android.app.Activity;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Bundle;
import android.util.Log;

import java.nio.ByteBuffer;

public class Main extends Activity {
    static final String TAG = "acl";

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        Log.i(TAG, run());
        System.exit(0);
    }

    static String fail(String step) {
        return "EGL14: FAILED at " + step + " (error 0x" + Integer.toHexString(EGL14.eglGetError())
                + ")";
    }

    static String run() {
        EGLDisplay d = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        if (d == EGL14.EGL_NO_DISPLAY) return fail("eglGetDisplay");
        int[] ver = new int[2];
        if (!EGL14.eglInitialize(d, ver, 0, ver, 1)) return fail("eglInitialize");
        int[] attrs = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_ALPHA_SIZE, 8, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
                EGL14.EGL_NONE};
        EGLConfig[] configs = new EGLConfig[1];
        int[] n = new int[1];
        if (!EGL14.eglChooseConfig(d, attrs, 0, configs, 0, 1, n, 0) || n[0] < 1)
            return fail("eglChooseConfig");
        int[] ctxAttrs = {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE};
        EGLContext c = EGL14.eglCreateContext(d, configs[0], EGL14.EGL_NO_CONTEXT, ctxAttrs, 0);
        if (c == EGL14.EGL_NO_CONTEXT) return fail("eglCreateContext");
        int[] pb = {EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE};
        EGLSurface s = EGL14.eglCreatePbufferSurface(d, configs[0], pb, 0);
        if (s == EGL14.EGL_NO_SURFACE) return fail("eglCreatePbufferSurface");
        if (!EGL14.eglMakeCurrent(d, s, s, c)) return fail("eglMakeCurrent");
        GLES20.glClearColor(0f, 1f, 0f, 1f);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        ByteBuffer px = ByteBuffer.allocateDirect(4);
        GLES20.glReadPixels(0, 0, 1, 1, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, px);
        String got = (px.get(0) & 0xff) + "," + (px.get(1) & 0xff) + "," + (px.get(2) & 0xff);
        EGL14.eglMakeCurrent(d, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
        EGL14.eglDestroySurface(d, s);
        EGL14.eglDestroyContext(d, c);
        return "EGL14: OK EGL " + ver[0] + "." + ver[1] + ", cleared pixel " + got;
    }
}
