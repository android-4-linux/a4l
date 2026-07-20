// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl test fixture; Android SDK docs (android.media.MediaCodec synchronous
// decode-to-Surface, ImageReader, MediaFormat). The end-to-end proof of acl's MediaCodec native
// underside ([[flutter-texture-bridge]] M5): a real MediaCodec decodes H.264 into an ImageReader's
// Surface and the decoded frame is acquired back — the zero-copy path video_player_android drives
// (media3/ExoPlayer -> MediaCodec.releaseOutputBuffer(render) -> the Texture's ImageReader). No GPL-3.0 sources.
package com.acl.mctest;

import android.app.Activity;
import android.graphics.ImageFormat;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Bundle;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;

public class Main extends Activity {
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        try {
            // The whole raw Annex-B H.264 elementary stream (SPS/PPS in-band) — bundled as an asset.
            InputStream is = getAssets().open("raw.h264");
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] tmp = new byte[8192];
            int n;
            while ((n = is.read(tmp)) > 0) bos.write(tmp, 0, n);
            byte[] data = bos.toByteArray();
            is.close();
            Log.i("acl", "MC decode: read " + data.length + "B of raw H.264");

            ImageReader r = ImageReader.newInstance(320, 240, ImageFormat.PRIVATE, 3,
                    HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE | HardwareBuffer.USAGE_CPU_WRITE_OFTEN);
            MediaCodec c = MediaCodec.createDecoderByType("video/avc");
            MediaFormat f = MediaFormat.createVideoFormat("video/avc", 320, 240);
            c.configure(f, r.getSurface(), null, 0);   // decode straight into the ImageReader Surface
            c.start();

            int inIdx = c.dequeueInputBuffer(10000);
            ByteBuffer ib = c.getInputBuffer(inIdx);
            ib.clear();
            ib.put(data);
            c.queueInputBuffer(inIdx, 0, data.length, 0, 0);
            int eos = c.dequeueInputBuffer(10000);
            c.queueInputBuffer(eos, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);  // drain

            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            boolean got = false, formatSeen = false;
            for (int i = 0; i < 200 && !got; i++) {
                int o = c.dequeueOutputBuffer(info, 20000);
                if (o == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat of = c.getOutputFormat();  // ExoPlayer reads this to size the surface
                    formatSeen = true;
                    Log.i("acl", "MC decode: output format " + of.getInteger(MediaFormat.KEY_WIDTH)
                            + "x" + of.getInteger(MediaFormat.KEY_HEIGHT));
                } else if (o >= 0) {
                    c.releaseOutputBuffer(o, true);      // render -> queue_imported into the ImageReader
                    Image img = r.acquireLatestImage();  // the decoded frame flowed to the consumer
                    if (img != null) {
                        got = true;
                        Log.i("acl", "MC decode: frame in ImageReader " + img.getWidth() + "x" + img.getHeight());
                        img.close();  // NB: not getHardwareBuffer() — that has a separate main-thread JNI bug
                    }
                }
            }
            Log.i("acl", "MC decode: formatChanged=" + formatSeen);
            c.stop();
            c.release();
            Log.i("acl", got ? "MC decode: PASS" : "MC decode: FAIL (no frame)");
        } catch (Throwable t) {
            Log.e("acl", "MC decode: FAILED", t);
        }
    }
}
