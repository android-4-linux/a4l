// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (android.media.MediaCodec asynchronous
// mode, android.view.SurfaceView, SurfaceHolder.Callback). No GPL-3.0 sources.
//
// A video on a SurfaceView, the way a player app shows one: the window has a SurfaceView at a
// known place, and a MediaCodec in asynchronous mode decodes an H.264 stream onto its Surface.
// a4l puts those frames on a subsurface below the window (the video plane). The stream is
// assets/clip.h264, Annex B with an access unit delimiter before every frame; it is played
// LOOPS times and the process then exits, so a test can read the totals.
package com.acl.videoplane;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
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

    private boolean started;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        // Nothing opaque lies under the video: no window background and none on the root, as
        // in a full-window player. a4l clears the video View's ancestors to transparent so the
        // plane below shows through; an ancestor's own background, drawn after that, would
        // cover it again.
        getWindow().setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        final FrameLayout root = new FrameLayout(this);
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

        // The SurfaceView joins the window after its first frames, as a player's does once
        // the app has something to play. a4l takes a SurfaceView that is there at launch for a
        // GL surface (a GLSurfaceView) and gives it the whole window; one that appears later is
        // a video surface.
        new Handler(getMainLooper()).postDelayed(new Runnable() {
            public void run() {
                SurfaceView sv = new SurfaceView(Main.this);
                FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(W, H);
                lp.leftMargin = LEFT;
                lp.topMargin = TOP;
                sv.getHolder().addCallback(new Callback());
                root.addView(sv, 0, lp);
            }
        }, 500);
    }

    private final class Callback implements SurfaceHolder.Callback {
        public void surfaceCreated(SurfaceHolder h) {
            if (started) return;
            started = true;
            Log.i(TAG, "VIDEO: surfaceCreated");
            try {
                play(h);
            } catch (Throwable e) {
                Log.e(TAG, "VIDEO: FAILED", e);
                System.exit(3);
            }
        }
        public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) {}
        public void surfaceDestroyed(SurfaceHolder h) {}
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
        final boolean[] done = {false};
        final Runnable finish = new Runnable() {
            public void run() {
                if (done[0]) return;
                done[0] = true;
                Log.i(TAG, "VIDEO: fed " + fed[0] + " rendered " + out[0]);
                System.exit(0);
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
                    Log.e(TAG, "VIDEO: onInput", e);
                }
            }
            public void onOutputBufferAvailable(MediaCodec mc, int idx, MediaCodec.BufferInfo info) {
                try {
                    mc.releaseOutputBuffer(idx, true);
                    // a4l paces the decoder to the stream's own clock, so the last frame
                    // comes out when it is due. Give it time to reach the screen.
                    if (++out[0] == total) handler.postDelayed(finish, 500);
                } catch (Throwable e) {
                    Log.e(TAG, "VIDEO: onOutput", e);
                }
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
