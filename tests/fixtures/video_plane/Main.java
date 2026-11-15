// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (android.media.MediaCodec asynchronous
// mode, android.view.SurfaceView, SurfaceHolder.Callback). No GPL-3.0 sources.
//
// A video on a SurfaceView, the way a player app shows one: the window has a SurfaceView at a
// known place, and a MediaCodec in asynchronous mode decodes an H.264 stream onto its Surface.
// a4l puts those frames on a subsurface below the window (the video plane). The stream is
// assets/clip.h264, Annex B with an access unit delimiter before every frame; it is played
// LOOPS times and the process then exits, so a test can read the totals.
//
// It does what a player does: each frame is released at the time it is due, since a decoder
// produces frames as fast as it is fed and showing them at the right moment is the app's job;
// and after the first loop it flushes the codec and starts it again, as a seek does.
package com.acl.videoplane;

import android.app.Activity;
import android.graphics.Color;
import android.media.MediaCodec;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;

public class Main extends Activity {
    static final String TAG = "acl";
    static final int W = 640, H = 360, LEFT = 120, TOP = 100, FPS = 30, LOOPS = 3;
    // The size the SurfaceView is laid out at again once playback has started, as a player
    // view does when it learns the picture's aspect ratio.
    static final int W2 = 560, H2 = 315;

    private boolean started;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        // An opaque background under the video, as most apps have: the SurfaceView has to
        // punch through it for the plane below the window to show.
        final FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.rgb(0x20, 0x30, 0x40));
        TextView label = new TextView(this);
        label.setText("a4l video plane");
        label.setTextColor(Color.WHITE);
        label.setTextSize(28);
        FrameLayout.LayoutParams tp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        tp.bottomMargin = 40;
        root.addView(label, tp);
        setContentView(root);

        // The SurfaceView is in the window from the start, as a player screen's is. MainLate
        // adds it after the first frames instead, as an app does that builds its player once
        // it has something to play. a4l must give both a video surface.
        final Runnable add = new Runnable() {
            public void run() {
                SurfaceView sv = new SurfaceView(Main.this);
                FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(W, H);
                lp.leftMargin = LEFT;
                lp.topMargin = TOP;
                sv.getHolder().addCallback(new Callback(sv));
                root.addView(sv, 0, lp);
            }
        };
        if (addLate()) new Handler(getMainLooper()).postDelayed(add, 500);
        else add.run();
    }

    /** Whether the SurfaceView joins the window after launch. */
    protected boolean addLate() {
        return false;
    }

    private final class Callback implements SurfaceHolder.Callback {
        private final SurfaceView sv;
        Callback(SurfaceView sv) {
            this.sv = sv;
        }
        public void surfaceCreated(SurfaceHolder h) {
            if (started) return;
            started = true;
            Log.i(TAG, "VIDEO: surfaceCreated");
            // Lay the view out again at another size while the video plays. The surface must
            // survive it: the app is told the new size and not that the surface is gone.
            new Handler(getMainLooper()).postDelayed(new Runnable() {
                public void run() {
                    FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) sv.getLayoutParams();
                    lp.width = W2;
                    lp.height = H2;
                    sv.setLayoutParams(lp);
                }
            }, 400);
            try {
                play(h);
            } catch (Throwable e) {
                Log.e(TAG, "VIDEO: FAILED", e);
                System.exit(3);
            }
        }
        public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) {
            Log.i(TAG, "VIDEO: surfaceChanged " + w + "x" + hh);
        }
        public void surfaceDestroyed(SurfaceHolder h) {
            Log.i(TAG, "VIDEO: surfaceDestroyed");
        }
    }

    // The offsets at which access units start: every access unit delimiter (NAL type 9).
    static ArrayList<Integer> accessUnits(byte[] d) {
        ArrayList<Integer> at = new ArrayList<>();
        for (int i = 0; i + 3 < d.length; i++) {
            if (d[i] != 0 || d[i + 1] != 0) continue;
            int nal;
            if (d[i + 2] == 1) nal = i + 3;
            else if (d[i + 2] == 0 && i + 4 < d.length && d[i + 3] == 1) nal = i + 4;
            else continue;
            if ((d[nal] & 0x1f) == 9) at.add(i);
            i = nal;
        }
        return at;
    }

    private void play(SurfaceHolder holder) throws Exception {
        InputStream is = getAssets().open("clip.h264");
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        byte[] t = new byte[8192];
        int n;
        while ((n = is.read(t)) > 0) bo.write(t, 0, n);
        is.close();
        final byte[] data = bo.toByteArray();
        final ArrayList<Integer> au = accessUnits(data);
        final int total = au.size() * LOOPS;
        Log.i(TAG, "VIDEO: " + au.size() + " access unit(s), playing " + total);
        if (au.isEmpty()) throw new IllegalStateException("no access units in clip.h264");

        MediaFormat f = MediaFormat.createVideoFormat("video/avc", W, H);
        String name = new MediaCodecList(MediaCodecList.REGULAR_CODECS).findDecoderForFormat(f);
        final MediaCodec c = MediaCodec.createByCodecName(name);
        final HandlerThread ht = new HandlerThread("video-cb");
        ht.start();
        final Handler handler = new Handler(ht.getLooper());
        final int[] fed = {0};
        final int[] out = {0};
        final int[] lost = {0};         // frames in the codec when it was flushed
        final int[] generation = {0};   // bumped by the flush: older buffers are void
        final boolean[] flushing = {false};
        final boolean[] flushed = {false};
        final long[] base = {0};        // System.nanoTime() at which pts 0 is due
        final boolean[] done = {false};
        final Runnable finish = new Runnable() {
            public void run() {
                if (done[0]) return;
                done[0] = true;
                Log.i(TAG, "VIDEO: fed " + fed[0] + " rendered " + out[0] + " lost " + lost[0]);
                System.exit(0);
            }
        };
        c.setCallback(new MediaCodec.Callback() {
            public void onInputBufferAvailable(final MediaCodec mc, int idx) {
                try {
                    if (flushing[0]) return;  // offered before the flush; void
                    if (!flushed[0] && fed[0] == au.size()) {
                        // One loop in: flush and start again, as a seek does. Buffers the
                        // codec handed out before this are void, and so are callbacks already
                        // on their way, which is why start() is posted behind them.
                        flushed[0] = true;
                        flushing[0] = true;
                        generation[0]++;
                        lost[0] = fed[0] - out[0];
                        Log.i(TAG, "VIDEO: flush with " + lost[0] + " frame(s) in the codec");
                        mc.flush();
                        handler.post(new Runnable() {
                            public void run() {
                                flushing[0] = false;
                                mc.start();
                                Log.i(TAG, "VIDEO: started after the flush");
                            }
                        });
                        return;
                    }
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
                    Log.e(TAG, "VIDEO: onInput", e);
                }
            }
            public void onOutputBufferAvailable(final MediaCodec mc, final int idx,
                    MediaCodec.BufferInfo info) {
                if (flushing[0]) return;  // decoded before the flush; void
                // Hold the buffer until its frame is due, then release it for that time.
                if (base[0] == 0) base[0] = System.nanoTime() + 50000000L - info.presentationTimeUs * 1000L;
                final long due = base[0] + info.presentationTimeUs * 1000L;
                final int gen = generation[0];
                final Runnable release = new Runnable() {
                    public void run() {
                        if (gen != generation[0]) return;  // flushed meanwhile
                        try {
                            mc.releaseOutputBuffer(idx, due);
                            // Give the last frame time to reach the screen.
                            if (++out[0] + lost[0] == total) handler.postDelayed(finish, 500);
                        } catch (Throwable e) {
                            Log.e(TAG, "VIDEO: onOutput", e);
                        }
                    }
                };
                long waitMs = (due - System.nanoTime()) / 1000000L - 20;
                if (waitMs > 0) handler.postDelayed(release, waitMs);
                else release.run();
            }
            public void onOutputFormatChanged(MediaCodec mc, MediaFormat fmt) {
                Log.i(TAG, "VIDEO: format " + fmt.getInteger(MediaFormat.KEY_WIDTH) + "x"
                        + fmt.getInteger(MediaFormat.KEY_HEIGHT));
            }
            public void onError(MediaCodec mc, MediaCodec.CodecException e) {
                Log.e(TAG, "VIDEO: codec error", e);
            }
        }, handler);
        c.configure(f, holder.getSurface(), null, 0);
        c.start();
        // A stream that never drains must not hang the run.
        handler.postDelayed(finish, (total * 1000L / FPS) + 15000);
    }
}
