// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (android.media.MediaCodec asynchronous
// mode, android.view.TextureView, android.graphics.SurfaceTexture, Surface(SurfaceTexture)). No
// GPL-3.0 sources.
//
// A video on a TextureView, the way a player app shows one there: a MediaCodec decodes a stream
// onto new Surface(surfaceTexture), and the TextureView draws each frame inside the window. The
// stream is assets/clip, H.264 or HEVC (the MIME type in assets/mime; H.264 without it), Annex B
// with an access unit delimiter before every frame, played LOOPS times, each on a codec of its
// own on the same Surface, as a player makes a new codec when it seeks. The process then exits so
// a test can read the totals and the last frame a4l rendered. Each frame is released at the time
// it is due, with that time, as a player does.
package com.acl.textureview;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.SurfaceTexture;
import android.media.MediaCodec;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;
import android.view.TextureView;
import android.widget.FrameLayout;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;

public class Main extends Activity {
    static final String TAG = "acl";
    static final int W = 640, H = 360, LEFT = 120, TOP = 100, FPS = 30, LOOPS = 2;

    private boolean started;
    private volatile int updated;     // onSurfaceTextureUpdated calls
    private volatile long timestamp;  // the SurfaceTexture's, at the last update

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        // A plain background, so a test can find the TextureView in the rendered window.
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.rgb(0x20, 0x30, 0x40));
        TextureView tv = new TextureView(this);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(W, H);
        lp.leftMargin = LEFT;
        lp.topMargin = TOP;
        tv.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
            public void onSurfaceTextureAvailable(SurfaceTexture st, int w, int h) {
                if (started) return;
                started = true;
                Log.i(TAG, "TEXTURE: available " + w + "x" + h);
                try {
                    play(new Surface(st));
                } catch (Throwable e) {
                    Log.e(TAG, "TEXTURE: FAILED", e);
                    System.exit(3);
                }
            }
            public void onSurfaceTextureSizeChanged(SurfaceTexture st, int w, int h) {}
            public boolean onSurfaceTextureDestroyed(SurfaceTexture st) {
                Log.i(TAG, "TEXTURE: destroyed");
                return true;
            }
            public void onSurfaceTextureUpdated(SurfaceTexture st) {
                updated++;
                timestamp = st.getTimestamp();
            }
        });
        root.addView(tv, lp);
        setContentView(root);
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

    private byte[] data;
    private ArrayList<Integer> au;
    private String mime;
    private Handler handler;
    private int fed, out;             // across every codec
    private long lastDue;             // the time the last frame was released for
    private boolean done;

    private void play(Surface surface) throws Exception {
        InputStream is = getAssets().open("clip");
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        byte[] t = new byte[8192];
        int n;
        while ((n = is.read(t)) > 0) bo.write(t, 0, n);
        is.close();
        data = bo.toByteArray();
        mime = mime();
        au = accessUnits(data, mime.equals("video/hevc"));
        final int total = au.size() * LOOPS;
        Log.i(TAG, "TEXTURE: " + au.size() + " access unit(s), playing " + total);
        if (au.isEmpty()) throw new IllegalStateException("no access units in the clip");
        HandlerThread ht = new HandlerThread("video-cb");
        ht.start();
        handler = new Handler(ht.getLooper());
        // A stream that never drains must not hang the run.
        handler.postDelayed(new Runnable() {
            public void run() {
                report();
            }
        }, (total * 1000L / FPS) + 15000);
        playLoop(surface, 0);
    }

    private void report() {
        if (done) return;
        done = true;
        Log.i(TAG, "TEXTURE: fed " + fed + " rendered " + out + " updated " + updated
                + " timestamp " + (timestamp == lastDue ? "matches" : "differs"));
        System.exit(0);
    }

    // One loop of the clip on a codec of its own, released when it is done, the way a player
    // makes a new codec when it seeks: the next loop makes another on the same Surface.
    private void playLoop(final Surface surface, final int loop) throws Exception {
        MediaFormat f = MediaFormat.createVideoFormat(mime, W, H);
        String name = new MediaCodecList(MediaCodecList.REGULAR_CODECS).findDecoderForFormat(f);
        final MediaCodec c = MediaCodec.createByCodecName(name);
        final int frames = au.size();
        final int[] loopFed = {0};
        final int[] loopOut = {0};
        final long[] base = {0};  // System.nanoTime() at which pts 0 is due
        c.setCallback(new MediaCodec.Callback() {
            public void onInputBufferAvailable(MediaCodec mc, int idx) {
                try {
                    if (loopFed[0] >= frames) {
                        if (loopFed[0] == frames) {
                            mc.queueInputBuffer(idx, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            loopFed[0]++;
                        }
                        return;
                    }
                    int i = loopFed[0];
                    int from = au.get(i);
                    int to = i + 1 < au.size() ? au.get(i + 1) : data.length;
                    ByteBuffer ib = mc.getInputBuffer(idx);
                    ib.clear();
                    ib.put(data, from, to - from);
                    mc.queueInputBuffer(idx, 0, to - from, i * 1000000L / FPS, 0);
                    loopFed[0]++;
                    fed++;
                } catch (Throwable e) {
                    Log.e(TAG, "TEXTURE: onInput", e);
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
                            lastDue = due;
                            out++;
                            if (++loopOut[0] == frames) next(mc);
                        } catch (Throwable e) {
                            Log.e(TAG, "TEXTURE: onOutput", e);
                        }
                    }
                };
                long waitMs = (due - System.nanoTime()) / 1000000L - 20;
                if (waitMs > 0) handler.postDelayed(release, waitMs);
                else release.run();
            }
            // This codec's loop is out: let it go once its last frame is due, then the next
            // loop, or the end. (a4l drops a frame still waiting for its time when its codec is
            // stopped.)
            private void next(final MediaCodec mc) {
                handler.postDelayed(new Runnable() {
                    public void run() {
                        mc.stop();
                        mc.release();
                        try {
                            if (loop + 1 < LOOPS) {
                                playLoop(surface, loop + 1);
                            } else {
                                // Give the last frame time to reach the screen.
                                handler.postDelayed(new Runnable() {
                                    public void run() {
                                        report();
                                    }
                                }, 700);
                            }
                        } catch (Throwable e) {
                            Log.e(TAG, "TEXTURE: FAILED", e);
                            System.exit(3);
                        }
                    }
                }, 100);
            }
            public void onOutputFormatChanged(MediaCodec mc, MediaFormat fmt) {
                Log.i(TAG, "TEXTURE: format " + fmt.getInteger(MediaFormat.KEY_WIDTH) + "x"
                        + fmt.getInteger(MediaFormat.KEY_HEIGHT));
            }
            public void onError(MediaCodec mc, MediaCodec.CodecException e) {
                Log.e(TAG, "TEXTURE: codec error", e);
            }
        }, handler);
        c.configure(f, surface, null, 0);
        c.start();
    }
}
