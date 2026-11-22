// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (android.graphics.SurfaceTexture,
// android.opengl.EGL14/GLES20, OES_EGL_image_external, android.media.MediaCodec asynchronous
// mode). No GPL-3.0 sources.
//
// A video sampled in the app's own GL, as a video effect or a 360-degree player does: a
// MediaCodec decodes assets/clip onto new Surface(new SurfaceTexture(tex)). The clip is H.264 or
// HEVC (the MIME type in assets/mime; H.264 without it), Annex B with an access unit delimiter
// before every frame. On each new frame, the app's GL thread calls updateTexImage and draws the
// external texture, through getTransformMatrix, into a pbuffer the picture's size, then reads
// back points in each quadrant: the middle, the corners of the picture and either side of the
// edges between quadrants. The clip is four colored quadrants, red, green, blue and white from
// top left to bottom right, inside a border its stream crops away; a crop not applied shows the
// border at the picture's edges or moves the edges between quadrants. Logs one line at the end
// with the totals and the matrix, and exits.
package com.acl.stgl;

import android.app.Activity;
import android.graphics.SurfaceTexture;
import android.media.MediaCodec;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.ArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public class Main extends Activity implements SurfaceTexture.OnFrameAvailableListener {
    static final String TAG = "acl";
    static final int W = 640, H = 360, FPS = 30, LOOPS = 2;
    static final int PW = W, PH = H;  // the pbuffer drawn into: one texel a pixel

    static final String VS = "attribute vec4 aPos;\n"
            + "attribute vec4 aTex;\n"
            + "uniform mat4 uM;\n"
            + "varying vec2 vTex;\n"
            + "void main() { gl_Position = aPos; vTex = (uM * aTex).xy; }\n";
    static final String FS = "#extension GL_OES_EGL_image_external : require\n"
            + "precision mediump float;\n"
            + "uniform samplerExternalOES uS;\n"
            + "varying vec2 vTex;\n"
            + "void main() { gl_FragColor = texture2D(uS, vTex); }\n";
    static final int[] RED = {255, 0, 0}, GREEN = {0, 255, 0}, BLUE = {0, 0, 255},
            WHITE = {255, 255, 255};
    // Expected colors at points of the picture, x right and y down from its top left, the way
    // the clip was drawn; read back at GL's (x, PH - 1 - y).
    static final Object[][] PROBES = {
        {PW / 4, PH / 4, RED}, {3 * PW / 4, PH / 4, GREEN},
        {PW / 4, 3 * PH / 4, BLUE}, {3 * PW / 4, 3 * PH / 4, WHITE},
        {1, 1, RED}, {PW - 2, 1, GREEN}, {1, PH - 2, BLUE}, {PW - 2, PH - 2, WHITE},
        {PW / 2 - 4, PH / 4, RED}, {PW / 2 + 4, PH / 4, GREEN},
        {PW / 4, PH / 2 - 4, RED}, {PW / 4, PH / 2 + 4, BLUE}};
    static final int TOLERANCE = 40;

    private Handler gl;
    private SurfaceTexture st;
    private int program, aPos, aTex, uM;
    private final float[] matrix = new float[16];
    private volatile int updated, right;
    private volatile String last = "none", matrixSeen = "none";
    private volatile long timestamp;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        // All of the work is on other threads; the main thread waits for it, so the run lasts
        // as long as the video with or without a window.
        HandlerThread t = new HandlerThread("gl");
        t.start();
        gl = new Handler(t.getLooper());
        final Surface[] surface = new Surface[1];
        final CountDownLatch ready = new CountDownLatch(1);
        gl.post(new Runnable() {
            public void run() {
                try {
                    surface[0] = setUpGl();
                } catch (Throwable e) {
                    Log.e(TAG, "STGL: FAILED", e);
                }
                ready.countDown();
            }
        });
        try {
            ready.await(10, TimeUnit.SECONDS);
            if (surface[0] == null) throw new IllegalStateException("no GL");
            play(surface[0]);
            // play()'s finish exits the process; this is the backstop.
            Thread.sleep(60000);
            Log.i(TAG, "STGL: timed out");
        } catch (Throwable e) {
            Log.e(TAG, "STGL: FAILED", e);
        }
        System.exit(3);
    }

    // On the GL thread: an EGL14 context current on a pbuffer, the program, and a
    // SurfaceTexture on a new external texture whose frames arrive on this thread.
    private Surface setUpGl() {
        EGLDisplay d = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] ver = new int[2];
        if (!EGL14.eglInitialize(d, ver, 0, ver, 1)) throw new IllegalStateException("eglInitialize");
        int[] attrs = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_ALPHA_SIZE, 8, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
                EGL14.EGL_NONE};
        EGLConfig[] configs = new EGLConfig[1];
        int[] n = new int[1];
        if (!EGL14.eglChooseConfig(d, attrs, 0, configs, 0, 1, n, 0) || n[0] < 1)
            throw new IllegalStateException("eglChooseConfig");
        EGLContext c = EGL14.eglCreateContext(d, configs[0], EGL14.EGL_NO_CONTEXT,
                new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
        EGLSurface s = EGL14.eglCreatePbufferSurface(d, configs[0],
                new int[] {EGL14.EGL_WIDTH, PW, EGL14.EGL_HEIGHT, PH, EGL14.EGL_NONE}, 0);
        if (!EGL14.eglMakeCurrent(d, s, s, c)) throw new IllegalStateException("eglMakeCurrent");
        program = GLES20.glCreateProgram();
        GLES20.glAttachShader(program, shader(GLES20.GL_VERTEX_SHADER, VS));
        GLES20.glAttachShader(program, shader(GLES20.GL_FRAGMENT_SHADER, FS));
        GLES20.glLinkProgram(program);
        int[] linked = new int[1];
        GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, linked, 0);
        if (linked[0] == 0) throw new IllegalStateException(GLES20.glGetProgramInfoLog(program));
        aPos = GLES20.glGetAttribLocation(program, "aPos");
        aTex = GLES20.glGetAttribLocation(program, "aTex");
        uM = GLES20.glGetUniformLocation(program, "uM");
        int[] tex = new int[1];
        GLES20.glGenTextures(1, tex, 0);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, tex[0]);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER,
                GLES20.GL_NEAREST);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER,
                GLES20.GL_NEAREST);
        st = new SurfaceTexture(tex[0]);
        st.setOnFrameAvailableListener(this, gl);
        return new Surface(st);
    }

    static int shader(int type, String src) {
        int sh = GLES20.glCreateShader(type);
        GLES20.glShaderSource(sh, src);
        GLES20.glCompileShader(sh);
        int[] ok = new int[1];
        GLES20.glGetShaderiv(sh, GLES20.GL_COMPILE_STATUS, ok, 0);
        if (ok[0] == 0) throw new IllegalStateException(GLES20.glGetShaderInfoLog(sh));
        return sh;
    }

    static FloatBuffer floats(float[] v) {
        FloatBuffer f = ByteBuffer.allocateDirect(v.length * 4).order(ByteOrder.nativeOrder())
                .asFloatBuffer();
        f.put(v).position(0);
        return f;
    }

    static final FloatBuffer QUAD = floats(new float[] {-1, -1, 1, -1, -1, 1, 1, 1});
    static final FloatBuffer UV = floats(new float[] {0, 0, 1, 0, 0, 1, 1, 1});

    // GL thread: latch the newest frame, draw it and read the probes back.
    @Override
    public void onFrameAvailable(SurfaceTexture t) {
        st.updateTexImage();
        st.getTransformMatrix(matrix);
        timestamp = st.getTimestamp();
        matrixSeen = matrix[0] + "," + matrix[5] + "," + matrix[12] + "," + matrix[13];
        GLES20.glViewport(0, 0, PW, PH);
        GLES20.glClearColor(0, 0, 0, 1);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        GLES20.glUseProgram(program);
        GLES20.glUniformMatrix4fv(uM, 1, false, matrix, 0);
        GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 0, QUAD);
        GLES20.glEnableVertexAttribArray(aPos);
        GLES20.glVertexAttribPointer(aTex, 2, GLES20.GL_FLOAT, false, 0, UV);
        GLES20.glEnableVertexAttribArray(aTex);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
        ByteBuffer px = ByteBuffer.allocateDirect(4);
        StringBuilder got = new StringBuilder();
        boolean ok = true;
        for (Object[] p : PROBES) {
            int x = (Integer) p[0], y = (Integer) p[1];
            int[] want = (int[]) p[2];
            px.clear();
            GLES20.glReadPixels(x, PH - 1 - y, 1, 1, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, px);
            int r = px.get(0) & 0xff, g = px.get(1) & 0xff, bl = px.get(2) & 0xff;
            boolean hit = Math.abs(r - want[0]) <= TOLERANCE && Math.abs(g - want[1]) <= TOLERANCE
                    && Math.abs(bl - want[2]) <= TOLERANCE;
            if (!hit) got.append(x).append(',').append(y).append('=');
            got.append(r).append(',').append(g).append(',').append(bl).append(' ');
            ok &= hit;
        }
        updated++;
        if (ok) right++;
        last = got.toString().trim();
    }

    // The stream's MIME type: assets/mime, or H.264 where there is none.
    String mime() {
        try {
            InputStream m = getAssets().open("mime");
            byte[] b = new byte[64];
            int n = m.read(b);
            m.close();
            return n > 0 ? new String(b, 0, n, "US-ASCII").trim() : "video/avc";
        } catch (java.io.IOException e) {
            return "video/avc";
        }
    }

    // The offsets at which access units start: every access unit delimiter (H.264 NAL type 9,
    // HEVC NAL type 35).
    static ArrayList<Integer> accessUnits(byte[] d, boolean hevc) {
        ArrayList<Integer> at = new ArrayList<>();
        for (int i = 0; i + 3 < d.length; i++) {
            if (d[i] != 0 || d[i + 1] != 0) continue;
            int nal;
            if (d[i + 2] == 1) nal = i + 3;
            else if (d[i + 2] == 0 && i + 4 < d.length && d[i + 3] == 1) nal = i + 4;
            else continue;
            int type = hevc ? (d[nal] >> 1) & 0x3f : d[nal] & 0x1f;
            if (type == (hevc ? 35 : 9)) at.add(i);
            i = nal;
        }
        return at;
    }

    private void play(Surface surface) throws Exception {
        InputStream is = getAssets().open("clip");
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        byte[] t = new byte[8192];
        int n;
        while ((n = is.read(t)) > 0) bo.write(t, 0, n);
        is.close();
        final byte[] data = bo.toByteArray();
        final String mime = mime();
        final ArrayList<Integer> au = accessUnits(data, mime.equals("video/hevc"));
        final int total = au.size() * LOOPS;
        if (au.isEmpty()) throw new IllegalStateException("no access units in the clip");

        MediaFormat f = MediaFormat.createVideoFormat(mime, W, H);
        String name = new MediaCodecList(MediaCodecList.REGULAR_CODECS).findDecoderForFormat(f);
        final MediaCodec c = MediaCodec.createByCodecName(name);
        final HandlerThread ht = new HandlerThread("video-cb");
        ht.start();
        final Handler handler = new Handler(ht.getLooper());
        final int[] fed = {0};
        final int[] out = {0};
        final long[] base = {0};
        final long[] lastDue = {0};
        final boolean[] done = {false};
        final Runnable finish = new Runnable() {
            public void run() {
                if (done[0]) return;
                done[0] = true;
                // Read on the GL thread, after any frame still being drawn there.
                gl.post(new Runnable() {
                    public void run() {
                        Log.i(TAG, "STGL: rendered " + out[0] + " updated " + updated + " right "
                                + right + " matrix " + matrixSeen + " timestamp "
                                + (timestamp == lastDue[0] ? "matches" : "differs")
                                + " last " + last);
                        System.exit(0);
                    }
                });
            }
        };
        c.setCallback(new MediaCodec.Callback() {
            public void onInputBufferAvailable(MediaCodec mc, int idx) {
                try {
                    if (fed[0] >= total) {
                        if (fed[0] == total) {
                            mc.queueInputBuffer(idx, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            fed[0]++;
                        }
                        return;
                    }
                    int i = fed[0] % au.size();
                    int from = au.get(i);
                    int to = i + 1 < au.size() ? au.get(i + 1) : data.length;
                    ByteBuffer ib = mc.getInputBuffer(idx);
                    ib.clear();
                    ib.put(data, from, to - from);
                    mc.queueInputBuffer(idx, 0, to - from, fed[0] * 1000000L / FPS, 0);
                    fed[0]++;
                } catch (Throwable e) {
                    Log.e(TAG, "STGL: onInput", e);
                }
            }
            public void onOutputBufferAvailable(final MediaCodec mc, final int idx,
                    MediaCodec.BufferInfo info) {
                if (base[0] == 0) base[0] = System.nanoTime() + 50000000L - info.presentationTimeUs * 1000L;
                final long due = base[0] + info.presentationTimeUs * 1000L;
                final Runnable release = new Runnable() {
                    public void run() {
                        try {
                            mc.releaseOutputBuffer(idx, due);
                            lastDue[0] = due;
                            if (++out[0] == total) handler.postDelayed(finish, 500);
                        } catch (Throwable e) {
                            Log.e(TAG, "STGL: onOutput", e);
                        }
                    }
                };
                long waitMs = (due - System.nanoTime()) / 1000000L - 20;
                if (waitMs > 0) handler.postDelayed(release, waitMs);
                else release.run();
            }
            public void onOutputFormatChanged(MediaCodec mc, MediaFormat fmt) {}
            public void onError(MediaCodec mc, MediaCodec.CodecException e) {
                Log.e(TAG, "STGL: codec error", e);
            }
        }, handler);
        c.configure(f, surface, null, 0);
        c.start();
        handler.postDelayed(finish, (total * 1000L / FPS) + 15000);
    }
}
