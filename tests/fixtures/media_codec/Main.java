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
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Bundle;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;

public class Main extends Activity {
    // The rendered frame reaches the ImageReader on the codec's own time, not
    // within releaseOutputBuffer, so the image is waited for (up to 2 s), as
    // an app waits for onImageAvailable. Taken at once, it was missed on a
    // loaded machine.
    static Image waitForImage(ImageReader r) throws InterruptedException {
        for (int i = 0; i < 200; i++) {
            Image img = r.acquireLatestImage();
            if (img != null) return img;
            Thread.sleep(10);
        }
        return null;
    }

    private static String meta(android.media.MediaMetadataRetriever r, int key) {
        return r.extractMetadata(key);
    }

    // MediaMetadataRetriever: what a player reads before it plays a file, and a gallery
    // before it shows one.
    private void retriever() {
        try {
            final java.io.File f = new java.io.File(getCacheDir(), "clip.mp4");
            try (InputStream in = getAssets().open("clip.mp4");
                    java.io.FileOutputStream os = new java.io.FileOutputStream(f)) {
                final byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
            }
            android.media.MediaMetadataRetriever r = new android.media.MediaMetadataRetriever();
            r.setDataSource(f.getPath());
            // 18, 19: video width and height; 9: duration in ms; 24: rotation; 12: MIME type;
            // 40: video codec MIME type; 17, 16: has video, has audio; 32: frame count.
            Log.i("acl", "MC retriever: file " + meta(r, 18) + "x" + meta(r, 19)
                    + " duration=" + meta(r, 9) + " rotation=" + meta(r, 24)
                    + " mime=" + meta(r, 12) + " codec=" + meta(r, 40)
                    + " video=" + meta(r, 17) + " audio=" + meta(r, 16)
                    + " frames=" + meta(r, 32) + " picture=" + r.getFrameAtTime());
            r.release();
            r = new android.media.MediaMetadataRetriever();
            try (android.content.res.AssetFileDescriptor afd = getAssets().openFd("turned.mp4")) {
                r.setDataSource(afd.getFileDescriptor(), afd.getStartOffset(), afd.getLength());
            }
            Log.i("acl", "MC retriever: asset " + meta(r, 18) + "x" + meta(r, 19)
                    + " duration=" + meta(r, 9) + " rotation=" + meta(r, 24));
            r.release();
            r.release();  // a second release does nothing
            boolean refused = false;
            r = new android.media.MediaMetadataRetriever();
            try {
                r.setDataSource(f.getPath() + ".none");
            } catch (RuntimeException e) {
                refused = meta(r, 9) == null;
            }
            r.release();
            Log.i("acl", "MC retriever: missing file refused=" + refused);
        } catch (Throwable t) {
            Log.e("acl", "MC retriever: FAIL " + t, t);
        }
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        retriever();
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
            MediaFormat f = MediaFormat.createVideoFormat("video/avc", 320, 240);
            // Discover the decoder the way media3/ExoPlayer does — MediaCodecList capability matching —
            // then create it by name (not createDecoderByType), the full ExoPlayer path.
            String codecName = new MediaCodecList(MediaCodecList.REGULAR_CODECS).findDecoderForFormat(f);
            Log.i("acl", "MC decode: findDecoderForFormat=" + codecName);
            MediaCodec c = MediaCodec.createByCodecName(codecName);
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
                            + "x" + of.getInteger(MediaFormat.KEY_HEIGHT)
                            + " crop " + of.getInteger("crop-left") + "," + of.getInteger("crop-top")
                            + "," + of.getInteger("crop-right") + "," + of.getInteger("crop-bottom"));
                } else if (o >= 0) {
                    c.releaseOutputBuffer(o, true);      // render -> queue_imported into the ImageReader
                    Image img = waitForImage(r);  // the decoded frame flowed to the consumer
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
