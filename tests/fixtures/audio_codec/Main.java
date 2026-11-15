// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (android.media.MediaCodec in its
// synchronous and asynchronous modes, MediaCodecList, MediaFormat, AudioTrack); ISO/IEC 13818-7
// for the ADTS header. No GPL-3.0 sources.
//
// AAC through MediaCodec to an AudioTrack, the way a media player plays a sound track: find a
// decoder for the format, feed it access units, and write the PCM it returns to a track.
//
// assets/tone.aac is an ADTS stream of a 440 Hz tone. The app cuts it into AAC frames, builds
// the AudioSpecificConfig the format carries as csd-0 from the first header, and decodes it
// twice: once to the end of the stream, and, after a flush, the first frames again. It
// measures what comes out (frames, the frequency of the left channel by zero crossings, the
// peak) and logs it for a test to read. Main drives the codec with dequeue calls; MainAsync
// with a MediaCodec.Callback.
package com.acl.audiocodec;

import android.app.Activity;
import android.graphics.Color;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.media.MediaCodec;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Gravity;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.ShortBuffer;
import java.util.ArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public class Main extends Activity {
    static final String TAG = "acl";
    static final String MIME = "audio/mp4a-latm";
    static final int[] RATES = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                                16000, 12000, 11025, 8000, 7350};
    static final int AFTER_FLUSH = 10;  // access units decoded again after the flush

    /** Whether to drive the codec with a callback. */
    protected boolean async() {
        return false;
    }

    // The stream.
    final ArrayList<byte[]> units = new ArrayList<>();
    byte[] csd;
    int rate, channelCount;

    // What came out.
    AudioTrack track;
    int outRate, outChannels, outEncoding;
    long frames, crossings;
    int peak, last;
    boolean sawFormat, eos;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        TextView label = new TextView(this);
        label.setText("a4l audio codec");
        label.setTextColor(Color.WHITE);
        label.setBackgroundColor(Color.rgb(0x20, 0x30, 0x40));
        label.setTextSize(28);
        label.setGravity(Gravity.CENTER);
        setContentView(label);
        new Thread(new Runnable() {
            public void run() {
                int rc = 3;
                try {
                    load();
                    if (async()) playAsync(); else playSync();
                    Log.i(TAG, "MC: done");
                    rc = 0;
                } catch (Throwable e) {
                    Log.e(TAG, "MC: FAILED", e);
                }
                System.exit(rc);
            }
        }, "player").start();
    }

    // Cut the ADTS stream into raw AAC frames and build csd-0 from the first header.
    void load() throws Exception {
        InputStream is = getAssets().open("tone.aac");
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        byte[] t = new byte[8192];
        int n;
        while ((n = is.read(t)) > 0) bo.write(t, 0, n);
        is.close();
        byte[] d = bo.toByteArray();
        int at = 0;
        while (at + 7 <= d.length) {
            if ((d[at] & 0xff) != 0xff || (d[at + 1] & 0xf0) != 0xf0)
                throw new IllegalStateException("not ADTS at " + at);
            int header = (d[at + 1] & 1) == 0 ? 9 : 7;
            int length = ((d[at + 3] & 3) << 11) | ((d[at + 4] & 0xff) << 3)
                    | ((d[at + 5] & 0xff) >> 5);
            if (length < header || at + length > d.length)
                throw new IllegalStateException("bad ADTS frame at " + at);
            if (csd == null) {
                int objectType = ((d[at + 2] & 0xff) >> 6) + 1;
                int freqIndex = ((d[at + 2] & 0xff) >> 2) & 0xf;
                int channelConfig = ((d[at + 2] & 1) << 2) | ((d[at + 3] & 0xff) >> 6);
                csd = new byte[] {(byte) ((objectType << 3) | (freqIndex >> 1)),
                                  (byte) (((freqIndex & 1) << 7) | (channelConfig << 3))};
                rate = RATES[freqIndex];
                channelCount = channelConfig;
            }
            byte[] unit = new byte[length - header];
            System.arraycopy(d, at + header, unit, 0, unit.length);
            units.add(unit);
            at += length;
        }
        Log.i(TAG, "MC: " + units.size() + " access units, " + rate + " Hz, "
                + channelCount + " ch in the stream");
    }

    MediaCodec makeCodec() throws Exception {
        MediaFormat f = MediaFormat.createAudioFormat(MIME, rate, channelCount);
        f.setByteBuffer("csd-0", ByteBuffer.wrap(csd));
        String name = new MediaCodecList(MediaCodecList.REGULAR_CODECS).findDecoderForFormat(f);
        Log.i(TAG, "MC: decoder " + name);
        if (name == null) throw new IllegalStateException("no decoder for " + MIME);
        MediaCodec c = MediaCodec.createByCodecName(name);
        if (async()) installCallback(c);
        c.configure(f, null, null, 0);
        return c;
    }

    void onFormat(MediaFormat f) {
        outRate = f.getInteger(MediaFormat.KEY_SAMPLE_RATE);
        outChannels = f.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
        outEncoding = f.containsKey(MediaFormat.KEY_PCM_ENCODING)
                ? f.getInteger(MediaFormat.KEY_PCM_ENCODING) : AudioFormat.ENCODING_PCM_16BIT;
        Log.i(TAG, "MC: format " + outRate + " Hz, " + outChannels + " ch, encoding "
                + outEncoding + ", mime " + f.getString(MediaFormat.KEY_MIME));
        sawFormat = true;
        if (track != null) return;
        int mask = outChannels == 1 ? AudioFormat.CHANNEL_OUT_MONO : AudioFormat.CHANNEL_OUT_STEREO;
        track = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(outRate)
                        .setChannelMask(mask).setEncoding(outEncoding).build())
                .setBufferSizeInBytes(AudioTrack.getMinBufferSize(outRate, mask, outEncoding))
                .setTransferMode(AudioTrack.MODE_STREAM).build();
        track.play();
    }

    // Measure a buffer of PCM and play it.
    void onPcm(ByteBuffer buf, MediaCodec.BufferInfo info) {
        if (!sawFormat) throw new IllegalStateException("a buffer before the format");
        if (info.size > 0) {
            buf.position(info.offset).limit(info.offset + info.size);
            ShortBuffer s = buf.duplicate().order(ByteOrder.nativeOrder()).asShortBuffer();
            int count = s.remaining();
            for (int i = 0; i < count; i += outChannels) {
                int v = s.get(i);
                if ((last < 0) != (v < 0)) crossings++;
                last = v;
                if (Math.abs(v) > peak) peak = Math.abs(v);
            }
            frames += count / outChannels;
            int wrote = track.write(buf, info.size, AudioTrack.WRITE_BLOCKING);
            if (wrote != info.size) throw new IllegalStateException("track wrote " + wrote);
        }
        if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) eos = true;
    }

    void report(String what) {
        double seconds = outRate > 0 ? (double) frames / outRate : 0;
        long hz = seconds > 0 ? Math.round(crossings / 2.0 / seconds) : 0;
        Log.i(TAG, "MC: " + what + " decoded " + frames + " frames, hz " + hz + ", peak "
                + peak + ", eos " + eos);
    }

    void reset() {
        frames = 0; crossings = 0; peak = 0; last = 0; eos = false;
    }

    void finishTrack() throws Exception {
        if (track == null) return;
        int head = 0;
        long want = totalPlayed;
        for (int i = 0; i < 300 && (head = track.getPlaybackHeadPosition()) < want; i++)
            Thread.sleep(10);
        Log.i(TAG, "MC: played head " + head + " of " + want);
        track.stop();
        track.release();
    }
    long totalPlayed;

    // ── synchronous ────────────────────────────────────────────────────────────────────

    void playSync() throws Exception {
        MediaCodec c = makeCodec();
        c.start();
        decodeSync(c, units.size(), true);
        report("first pass");
        totalPlayed += frames;
        // A seek: flush, then decode the start of the stream again.
        c.flush();
        reset();
        decodeSync(c, AFTER_FLUSH, false);
        report("after flush");
        totalPlayed += frames;
        finishTrack();
        c.stop();
        c.release();
    }

    void decodeSync(MediaCodec c, int count, boolean toEnd) throws Exception {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        int fed = 0;
        boolean inputDone = false;
        long deadline = System.currentTimeMillis() + 20000;
        int idle = 0;
        while (System.currentTimeMillis() < deadline) {
            if (!inputDone) {
                int in = c.dequeueInputBuffer(0);
                if (in >= 0) {
                    if (fed < count) {
                        byte[] u = units.get(fed);
                        ByteBuffer ib = c.getInputBuffer(in);
                        ib.clear();
                        ib.put(u);
                        c.queueInputBuffer(in, 0, u.length, fed * 1024L * 1000000L / rate, 0);
                        fed++;
                    } else if (toEnd) {
                        c.queueInputBuffer(in, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        inputDone = true;
                    } else {
                        // Not ending the stream: hand the buffer back empty and stop feeding.
                        c.queueInputBuffer(in, 0, 0, 0, 0);
                        inputDone = true;
                    }
                }
            }
            int out = c.dequeueOutputBuffer(info, 10000);
            if (out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                onFormat(c.getOutputFormat());
            } else if (out >= 0) {
                onPcm(c.getOutputBuffer(out), info);
                c.releaseOutputBuffer(out, false);
                idle = 0;
                if (eos) return;
            } else if (inputDone && !toEnd && ++idle > 20) {
                return;  // everything fed has come out
            }
        }
        throw new IllegalStateException("decode timed out");
    }

    // ── asynchronous ───────────────────────────────────────────────────────────────────

    final Object lock = new Object();
    int asyncFed, asyncCount;
    boolean asyncToEnd, asyncInputDone;
    CountDownLatch asyncDone;
    Throwable asyncError;
    long lastOutputMs;

    void installCallback(MediaCodec codec) {
        HandlerThread ht = new HandlerThread("codec-cb");
        ht.start();
        codec.setCallback(new MediaCodec.Callback() {
            public void onInputBufferAvailable(MediaCodec c, int in) {
                try {
                    synchronized (lock) {
                        if (asyncInputDone) return;  // keep the buffer; nothing more to feed
                        if (asyncFed < asyncCount) {
                            byte[] u = units.get(asyncFed);
                            ByteBuffer ib = c.getInputBuffer(in);
                            ib.clear();
                            ib.put(u);
                            c.queueInputBuffer(in, 0, u.length,
                                    asyncFed * 1024L * 1000000L / rate, 0);
                            asyncFed++;
                        } else {
                            c.queueInputBuffer(in, 0, 0, 0,
                                    asyncToEnd ? MediaCodec.BUFFER_FLAG_END_OF_STREAM : 0);
                            asyncInputDone = true;
                        }
                    }
                } catch (Throwable e) { fail(e); }
            }
            public void onOutputBufferAvailable(MediaCodec c, int out, MediaCodec.BufferInfo info) {
                try {
                    onPcm(c.getOutputBuffer(out), info);
                    c.releaseOutputBuffer(out, false);
                    lastOutputMs = System.currentTimeMillis();
                    if (eos) asyncDone.countDown();
                } catch (Throwable e) { fail(e); }
            }
            public void onOutputFormatChanged(MediaCodec c, MediaFormat f) {
                try { onFormat(f); } catch (Throwable e) { fail(e); }
            }
            public void onError(MediaCodec c, MediaCodec.CodecException e) { fail(e); }
            void fail(Throwable e) {
                asyncError = e;
                asyncDone.countDown();
            }
        }, new Handler(ht.getLooper()));
    }

    void playAsync() throws Exception {
        MediaCodec c = makeCodec();
        synchronized (lock) {
            asyncFed = 0; asyncCount = units.size(); asyncToEnd = true; asyncInputDone = false;
            asyncDone = new CountDownLatch(1);
        }
        c.start();
        if (!asyncDone.await(20, TimeUnit.SECONDS)) throw new IllegalStateException("timed out");
        if (asyncError != null) throw new IllegalStateException(asyncError);
        report("first pass");
        totalPlayed += frames;

        // A seek: flush, then start() again, which is what resumes the callbacks.
        c.flush();
        synchronized (lock) {
            reset();
            asyncFed = 0; asyncCount = AFTER_FLUSH; asyncToEnd = false; asyncInputDone = false;
            asyncDone = new CountDownLatch(1);
            lastOutputMs = System.currentTimeMillis();
        }
        c.start();
        // No end of stream this time: wait until output has stopped arriving.
        long deadline = System.currentTimeMillis() + 20000;
        while (System.currentTimeMillis() < deadline) {
            Thread.sleep(50);
            synchronized (lock) {
                if (asyncInputDone && System.currentTimeMillis() - lastOutputMs > 400) break;
            }
        }
        if (asyncError != null) throw new IllegalStateException(asyncError);
        report("after flush");
        totalPlayed += frames;
        finishTrack();
        c.stop();
        c.release();
    }
}
