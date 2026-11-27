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
                    record();
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

    /**
     * An AudioRecord starts, paces a blocking read by the clock, and stops. a4l opens no
     * microphone, so what it reads is silence: 100 ms of it takes about 100 ms.
     */
    static void record() throws Exception {
        final int rate = 16000;
        int min = android.media.AudioRecord.getMinBufferSize(rate, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        int bad = android.media.AudioRecord.getMinBufferSize(100, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        android.media.AudioRecord r = new android.media.AudioRecord(
                android.media.MediaRecorder.AudioSource.VOICE_COMMUNICATION, rate,
                AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, min * 2);
        int state = r.getState();
        r.startRecording();
        boolean recording =
                r.getRecordingState() == android.media.AudioRecord.RECORDSTATE_RECORDING;
        short[] pcm = new short[rate / 10];
        java.util.Arrays.fill(pcm, (short) 0x1234);
        long t0 = System.nanoTime();
        int n = r.read(pcm, 0, pcm.length);
        long readMs = (System.nanoTime() - t0) / 1000000;
        boolean silent = true;
        for (short v : pcm) silent &= v == 0;
        // The same stream into a direct buffer, and the time of the last frame read.
        ByteBuffer direct = ByteBuffer.allocateDirect(640).order(ByteOrder.nativeOrder());
        for (int i = 0; i < 640; i++) direct.put(i, (byte) 0x55);
        int inBuffer = r.read(direct, 640);
        for (int i = 0; i < 640; i++) silent &= direct.get(i) == 0;
        android.media.AudioTimestamp ts = new android.media.AudioTimestamp();
        int stamped = r.getTimestamp(ts, android.media.AudioTimestamp.TIMEBASE_MONOTONIC);
        boolean timed = stamped == android.media.AudioRecord.SUCCESS
                && ts.framePosition == n + inBuffer / 2 && ts.nanoTime > 0;
        r.stop();
        boolean stopped =
                r.getRecordingState() == android.media.AudioRecord.RECORDSTATE_STOPPED;
        r.release();

        // Float and 8-bit: silence is 0.0 and, unsigned, 128.
        android.media.AudioRecord f = new android.media.AudioRecord(
                android.media.MediaRecorder.AudioSource.MIC, rate, AudioFormat.CHANNEL_IN_STEREO,
                AudioFormat.ENCODING_PCM_FLOAT, 8 * rate / 10);
        f.startRecording();
        float[] floats = new float[640];
        java.util.Arrays.fill(floats, 0.5f);
        int nf = f.read(floats, 0, floats.length, android.media.AudioRecord.READ_BLOCKING);
        for (float v : floats) silent &= v == 0f;
        f.release();
        android.media.AudioRecord e = new android.media.AudioRecord(
                android.media.MediaRecorder.AudioSource.MIC, rate, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_8BIT, rate / 10);
        e.startRecording();
        byte[] bytes = new byte[320];
        int nb = e.read(bytes, 0, bytes.length);
        for (byte v : bytes) silent &= v == (byte) 128;
        e.release();
        Log.i(TAG, "AT: record formats buffer " + inBuffer + " timed " + timed + " float " + nf
                + " bytes " + nb + " after release " + e.read(bytes, 0, bytes.length));
        Log.i(TAG, "AT: record min " + min + " bad " + bad + " state " + state + " read " + n
                + " in " + readMs + " ms silent " + silent + " recording " + recording
                + " stopped " + stopped);
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
