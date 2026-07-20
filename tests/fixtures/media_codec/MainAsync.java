// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl test fixture; Android SDK docs (android.media.MediaCodec ASYNCHRONOUS
// mode — setCallback + MediaCodec.Callback, HandlerThread). The async-path proof of acl's MediaCodec
// native underside ([[flutter-texture-bridge]] M5): the codec's driver thread delivers
// onInputBufferAvailable / onOutputFormatChanged / onOutputBufferAvailable, and H.264 decodes into an
// ImageReader Surface via releaseOutputBuffer(render) — the async path media3/ExoPlayer uses on
// configs that enable asynchronous operation. No GPL-3.0 sources.
package com.acl.mctest;

import android.app.Activity;
import android.graphics.ImageFormat;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodec;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public class MainAsync extends Activity {
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        try {
            InputStream is = getAssets().open("raw.h264");
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            byte[] t = new byte[8192];
            int n;
            while ((n = is.read(t)) > 0) bo.write(t, 0, n);
            final byte[] data = bo.toByteArray();
            is.close();

            final ImageReader r = ImageReader.newInstance(320, 240, ImageFormat.PRIVATE, 3,
                    HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE | HardwareBuffer.USAGE_CPU_WRITE_OFTEN);
            MediaFormat f = MediaFormat.createVideoFormat("video/avc", 320, 240);
            String name = new MediaCodecList(MediaCodecList.REGULAR_CODECS).findDecoderForFormat(f);
            final MediaCodec c = MediaCodec.createByCodecName(name);
            final CountDownLatch latch = new CountDownLatch(1);
            final boolean[] got = {false};
            final int[] inCount = {0};
            final HandlerThread ht = new HandlerThread("acl-cb");
            ht.start();
            c.setCallback(new MediaCodec.Callback() {  // non-null callback -> ASYNCHRONOUS mode
                public void onInputBufferAvailable(MediaCodec mc, int idx) {
                    try {
                        if (inCount[0] == 0) {
                            ByteBuffer ib = mc.getInputBuffer(idx);
                            ib.clear();
                            ib.put(data);
                            mc.queueInputBuffer(idx, 0, data.length, 0, 0);
                        } else if (inCount[0] == 1) {
                            mc.queueInputBuffer(idx, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        }
                        inCount[0]++;
                    } catch (Throwable e) { Log.e("acl", "onInput", e); }
                }
                public void onOutputBufferAvailable(MediaCodec mc, int idx, MediaCodec.BufferInfo info) {
                    try {
                        mc.releaseOutputBuffer(idx, true);   // render -> queue_imported into the ImageReader
                        Image img = r.acquireLatestImage();
                        if (img != null) {
                            if (!got[0]) {
                                got[0] = true;
                                Log.i("acl", "ASYNC: frame in ImageReader " + img.getWidth() + "x" + img.getHeight());
                                latch.countDown();
                            }
                            img.close();
                        }
                    } catch (Throwable e) { Log.e("acl", "onOutput", e); }
                }
                public void onOutputFormatChanged(MediaCodec mc, MediaFormat fmt) {
                    Log.i("acl", "ASYNC: onOutputFormatChanged " + fmt.getInteger(MediaFormat.KEY_WIDTH)
                            + "x" + fmt.getInteger(MediaFormat.KEY_HEIGHT));
                }
                public void onError(MediaCodec mc, MediaCodec.CodecException e) { Log.e("acl", "ASYNC onError", e); }
            }, new Handler(ht.getLooper()));
            c.configure(f, r.getSurface(), null, 0);
            c.start();
            latch.await(10, TimeUnit.SECONDS);
            c.stop();
            c.release();
            ht.quitSafely();
            Log.i("acl", got[0] ? "ASYNC test: PASS" : "ASYNC test: FAIL");
            finish();
        } catch (Throwable e) {
            Log.e("acl", "ASYNC test: FAILED", e);
        }
    }
}
