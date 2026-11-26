// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (android.media.AudioTrack,
// AudioFormat, AudioAttributes, AudioTimestamp). No GPL-3.0 sources.
//
// PCM through AudioTrack, the way an app that makes its own sound does it, and the way a media
// player delivers decoded audio: a streaming track, blocking writes from a thread, and the
// playback position read back. Two tracks, one after the other:
//
//   A  48 kHz stereo 16-bit: 1 s of a 440 Hz tone as short[], 0.5 s as byte[], 0.5 s as a
//      direct ByteBuffer.
//   B  44.1 kHz mono float: 0.5 s of an 880 Hz tone as float[].
//
// Each line it logs is what a test reads. A blocking write must take about as long as the
// audio it writes, less what the track's buffer holds; that is what "blocking" means, and a
// player depends on it to pace itself.
package com.acl.audiotrack;

import android.app.Activity;
import android.graphics.Color;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTimestamp;
import android.media.AudioTrack;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.widget.TextView;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class Main extends Activity {
    static final String TAG = "acl";

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        TextView label = new TextView(this);
        label.setText("a4l audio track");
        label.setTextColor(Color.WHITE);
        label.setBackgroundColor(Color.rgb(0x20, 0x30, 0x40));
        label.setTextSize(28);
        label.setGravity(Gravity.CENTER);
        setContentView(label);

        new Thread(new Runnable() {
            public void run() {
                int rc = 3;
                try {
                    trackA();
                    trackB();
                    tone();
                    Log.i(TAG, "AT: done");
                    rc = 0;
                } catch (Throwable e) {
                    Log.e(TAG, "AT: FAILED", e);
                }
                System.exit(rc);
            }
        }, "audio").start();
    }

    static AudioTrack make(int rate, int channelMask, int encoding) {
        int min = AudioTrack.getMinBufferSize(rate, channelMask, encoding);
        Log.i(TAG, "AT: minBufferSize " + min + " for " + rate + " Hz");
        if (min <= 0) throw new IllegalStateException("getMinBufferSize " + min);
        AudioTrack t = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build())
                .setAudioFormat(new AudioFormat.Builder()
                        .setSampleRate(rate)
                        .setChannelMask(channelMask)
                        .setEncoding(encoding)
                        .build())
                .setBufferSizeInBytes(min)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build();
        if (t.getState() != AudioTrack.STATE_INITIALIZED)
            throw new IllegalStateException("state " + t.getState());
        Log.i(TAG, "AT: track " + t.getSampleRate() + " Hz, " + t.getChannelCount()
                + " ch, buffer " + t.getBufferSizeInFrames() + " frames");
        return t;
    }

    // Wait for the playback position to reach `frames`, or two seconds.
    static int awaitHead(AudioTrack t, int frames) throws InterruptedException {
        int head = 0;
        for (int i = 0; i < 200; i++) {
            head = t.getPlaybackHeadPosition();
            if (head >= frames) break;
            Thread.sleep(10);
        }
        return head;
    }

    static void trackA() throws Exception {
        final int rate = 48000;
        AudioTrack t = make(rate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        short[] tone = new short[2 * rate * 2];  // 2 s, stereo
        for (int i = 0; i < 2 * rate; i++) {
            short s = (short) (Math.sin(2 * Math.PI * 440 * i / rate) * 8000);
            tone[2 * i] = s;
            tone[2 * i + 1] = s;
        }
        t.play();
        if (t.getPlayState() != AudioTrack.PLAYSTATE_PLAYING)
            throw new IllegalStateException("play state " + t.getPlayState());
        long t0 = System.nanoTime();
        int frames = 0;

        // 1 s as short[], in 10 ms writes.
        final int chunk = rate / 100 * 2;
        for (int at = 0; at < rate * 2; at += chunk) {
            int n = t.write(tone, at, chunk);
            if (n != chunk) throw new IllegalStateException("short write " + n);
            frames += n / 2;
        }
        // 0.5 s as byte[].
        byte[] bytes = new byte[rate / 2 * 4];
        ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer()
                .put(tone, rate * 2, rate / 2 * 2);
        int n = t.write(bytes, 0, bytes.length);
        if (n != bytes.length) throw new IllegalStateException("byte write " + n);
        frames += n / 4;
        // 0.5 s as a direct ByteBuffer.
        ByteBuffer direct = ByteBuffer.allocateDirect(rate / 2 * 4).order(ByteOrder.nativeOrder());
        direct.asShortBuffer().put(tone, rate * 3, rate / 2 * 2);
        n = t.write(direct, direct.remaining(), AudioTrack.WRITE_BLOCKING);
        if (n != rate / 2 * 4) throw new IllegalStateException("buffer write " + n);
        frames += n / 4;

        long writeMs = (System.nanoTime() - t0) / 1000000;
        Log.i(TAG, "AT: A wrote " + frames + " frames in " + writeMs + " ms");
        int head = awaitHead(t, frames);
        AudioTimestamp ts = new AudioTimestamp();
        boolean haveTs = t.getTimestamp(ts);
        Log.i(TAG, "AT: A head " + head + " of " + frames + ", timestamp "
                + (haveTs ? ts.framePosition : -1) + ", underruns " + t.getUnderrunCount());
        t.stop();
        if (t.getPlayState() != AudioTrack.PLAYSTATE_STOPPED)
            throw new IllegalStateException("play state after stop " + t.getPlayState());
        t.release();
    }

    /**
     * A ToneGenerator plays a tone that ends by itself: 1200 Hz, 100 ms on and 100 ms off,
     * twice. The sink's frame count on release says the whole tone went out.
     */
    static void tone() throws Exception {
        android.media.ToneGenerator tg = new android.media.ToneGenerator(
                android.media.AudioManager.STREAM_MUSIC, 80);
        boolean bad = tg.startTone(999);
        boolean started = tg.startTone(android.media.ToneGenerator.TONE_PROP_ACK);
        Thread.sleep(1000);
        tg.stopTone();
        int session = tg.getAudioSessionId();
        tg.release();
        tg.stopTone(); // after release: ignored
        Log.i(TAG, "AT: tone started " + started + ", unknown " + bad + ", session "
                + (session != 0) + ", after release " + tg.startTone(0));
    }

    static void trackB() throws Exception {
        final int rate = 44100;
        AudioTrack t = make(rate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_FLOAT);
        float[] tone = new float[rate / 2];
        for (int i = 0; i < tone.length; i++)
            tone[i] = (float) (Math.sin(2 * Math.PI * 880 * i / rate) * 0.25);
        t.setVolume(0.5f);
        t.play();
        long t0 = System.nanoTime();
        int n = t.write(tone, 0, tone.length, AudioTrack.WRITE_BLOCKING);
        if (n != tone.length) throw new IllegalStateException("float write " + n);
        long writeMs = (System.nanoTime() - t0) / 1000000;
        Log.i(TAG, "AT: B wrote " + n + " frames in " + writeMs + " ms");
        int head = awaitHead(t, n);
        Log.i(TAG, "AT: B head " + head + " of " + n);
        t.pause();
        t.flush();
        t.stop();
        t.release();
    }
}
