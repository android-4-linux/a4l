// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl test fixture; Android SDK TextureView / EGL10 / GLES20 / GLUtils docs.
// No GPL-3.0 sources.
//
// Minimum-viable repro of the SurfaceTexture -> present path (the TextureView GL producer acl bridges
// for mixed Java-EGL/native-GL apps). A full-screen TextureView; on SurfaceTextureAvailable a render
// thread sets up EGL10 on the SurfaceTexture (eglCreateWindowSurface -> acl's producer), uploads a
// Canvas-drawn Bitmap as a GL texture via GLUtils.texImage2D (the exact path Telegram's intro uses),
// compiles a shader program, and loops drawing a SPINNING full-screen textured quad (glDrawArrays)
// + eglSwapBuffers. Exercises the whole GLES2 texture pipeline: glGenTextures/glBindTexture/
// GLUtils.texImage2D/glTexParameteri, glCreateShader/glShaderSource/glCompileShader/glCreateProgram/
// glAttachShader/glLinkProgram, glGetAttribLocation/glGetUniformLocation, glVertexAttribPointer/
// glDrawArrays, PLUS acl's eglCreateWindowSurfaceTexture, the eglMakeCurrent/GLES20/eglSwapBuffers
// donor-%fs routing, the gbm readback, the SurfaceTexture feed, and the TextureLayer composite.
package com.acl.gltex;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.SurfaceTexture;
import android.opengl.GLES20;
import android.opengl.GLUtils;
import android.os.Bundle;
import android.util.Log;
import android.view.TextureView;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;

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
        if (thread != null) { thread.w = w; thread.h = h; }
    }

    @Override
    public boolean onSurfaceTextureDestroyed(SurfaceTexture st) {
        if (thread != null) { thread.running = false; thread = null; }
        return true;
    }

    @Override
    public void onSurfaceTextureUpdated(SurfaceTexture st) {}

    // A recognizable 256x256 texture: four color quadrants + a white ring, drawn with a Canvas.
    static Bitmap makeTexture() {
        Bitmap bmp = Bitmap.createBitmap(256, 256, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(bmp);
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setColor(Color.rgb(220, 40, 40)); c.drawRect(0, 0, 128, 128, p);      // TL red
        p.setColor(Color.rgb(40, 180, 60)); c.drawRect(128, 0, 256, 128, p);    // TR green
        p.setColor(Color.rgb(40, 90, 220)); c.drawRect(0, 128, 128, 256, p);    // BL blue
        p.setColor(Color.rgb(230, 200, 40)); c.drawRect(128, 128, 256, 256, p); // BR yellow
        p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(16); p.setColor(Color.WHITE);
        c.drawCircle(128, 128, 96, p);
        return bmp;
    }

    static int compile(int type, String src) {
        int s = GLES20.glCreateShader(type);
        GLES20.glShaderSource(s, src);
        GLES20.glCompileShader(s);
        int[] ok = new int[1];
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0);
        if (ok[0] == 0) Log.e(TAG, "shader compile failed: " + GLES20.glGetShaderInfoLog(s));
        return s;
    }

    static FloatBuffer floats(float[] a) {
        FloatBuffer fb = ByteBuffer.allocateDirect(a.length * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
        fb.put(a).position(0);
        return fb;
    }

    static final String VERT =
        "attribute vec2 aPos; attribute vec2 aTex; uniform float uAngle; varying vec2 vTex;\n" +
        "void main(){ float c=cos(uAngle), s=sin(uAngle); vec2 t=aTex-0.5;\n" +
        "  vTex = vec2(t.x*c - t.y*s, t.x*s + t.y*c) + 0.5; gl_Position=vec4(aPos,0.0,1.0); }";
    static final String FRAG =
        "precision mediump float; uniform sampler2D uTex; varying vec2 vTex;\n" +
        "void main(){ gl_FragColor = texture2D(uTex, vTex); }";

    static class RenderThread extends Thread {
        final SurfaceTexture st;
        volatile int w, h;
        volatile boolean running = true;

        RenderThread(SurfaceTexture st, int w, int h) { this.st = st; this.w = w; this.h = h; }

        @Override
        public void run() {
            final int EGL_OPENGL_ES2_BIT = 0x0004, EGL_CONTEXT_CLIENT_VERSION = 0x3098;
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
            if (!egl.eglChooseConfig(dpy, cfgAttr, cfgs, 1, num) || num[0] == 0) { Log.e(TAG, "eglChooseConfig failed"); return; }
            int[] ctxAttr = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL10.EGL_NONE };
            EGLContext ctx = egl.eglCreateContext(dpy, cfgs[0], EGL10.EGL_NO_CONTEXT, ctxAttr);
            EGLSurface sfc = egl.eglCreateWindowSurface(dpy, cfgs[0], st, null);
            if (sfc == null || sfc == EGL10.EGL_NO_SURFACE) { Log.e(TAG, "eglCreateWindowSurface(SurfaceTexture) failed"); return; }
            egl.eglMakeCurrent(dpy, sfc, sfc, ctx);
            Log.i(TAG, "GL context ready (" + GLES20.glGetString(GLES20.GL_RENDERER) + ")");

            // Texture upload via GLUtils.texImage2D (the Bitmap->GL path Telegram's intro uses).
            int[] tex = new int[1];
            GLES20.glGenTextures(1, tex, 0);
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, tex[0]);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
            GLUtils.texImage2D(GLES20.GL_TEXTURE_2D, 0, makeTexture(), 0);
            Log.i(TAG, "texture uploaded via GLUtils.texImage2D");

            int prog = GLES20.glCreateProgram();
            GLES20.glAttachShader(prog, compile(GLES20.GL_VERTEX_SHADER, VERT));
            GLES20.glAttachShader(prog, compile(GLES20.GL_FRAGMENT_SHADER, FRAG));
            GLES20.glLinkProgram(prog);
            GLES20.glUseProgram(prog);
            int aPos = GLES20.glGetAttribLocation(prog, "aPos");
            int aTex = GLES20.glGetAttribLocation(prog, "aTex");
            int uAngle = GLES20.glGetUniformLocation(prog, "uAngle");
            int uTex = GLES20.glGetUniformLocation(prog, "uTex");
            // Full-screen quad (triangle strip) + texcoords.
            FloatBuffer pos = floats(new float[]{ -1, -1,  1, -1, -1, 1,  1, 1 });
            FloatBuffer uv  = floats(new float[]{  0,  1,  1,  1,  0, 0,  1, 0 });
            GLES20.glEnableVertexAttribArray(aPos);
            GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 0, pos);
            GLES20.glEnableVertexAttribArray(aTex);
            GLES20.glVertexAttribPointer(aTex, 2, GLES20.GL_FLOAT, false, 0, uv);
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, tex[0]);
            GLES20.glUniform1i(uTex, 0);

            long t0 = System.currentTimeMillis();
            int frame = 0;
            while (running) {
                float t = (System.currentTimeMillis() - t0) / 1000f;
                GLES20.glViewport(0, 0, w, h);
                GLES20.glClearColor(0.08f, 0.08f, 0.10f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                GLES20.glUniform1f(uAngle, t);  // spin the texture
                GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
                egl.eglSwapBuffers(dpy, sfc);
                if (frame++ % 60 == 0) Log.i(TAG, "rendered frame " + frame);
                try { Thread.sleep(16); } catch (InterruptedException e) {}
            }
        }
    }
}
