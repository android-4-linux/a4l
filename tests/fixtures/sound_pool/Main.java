// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (android.media.SoundPool,
// AssetFileDescriptor). No GPL-3.0 sources.
//
// A sound through SoundPool, the way a game plays an effect: load assets/tone.wav, play it
// once it has loaded, and exit when it has had time to finish. Under `a4l run --split` the
// samples are mixed in the app process and played by the broker; a test reads the broker's
// count.
package com.acl.soundpool;

import android.app.Activity;
import android.content.res.AssetFileDescriptor;
import android.graphics.Color;
import android.media.AudioAttributes;
import android.media.SoundPool;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.view.Gravity;
import android.widget.TextView;

public class Main extends Activity {
    static final String TAG = "acl";
    static final int PLAY_MS = 2500;  // the tone is 1 s; played twice

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        TextView label = new TextView(this);
        label.setText("a4l sound pool");
        label.setTextColor(Color.WHITE);
        label.setBackgroundColor(Color.rgb(0x20, 0x30, 0x40));
        label.setTextSize(28);
        label.setGravity(Gravity.CENTER);
        setContentView(label);

        final Handler handler = new Handler(getMainLooper());
        try {
            final SoundPool pool = new SoundPool.Builder()
                    .setMaxStreams(2)
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_GAME)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                            .build())
                    .build();
            pool.setOnLoadCompleteListener(new SoundPool.OnLoadCompleteListener() {
                public void onLoadComplete(SoundPool sp, int sampleId, int status) {
                    Log.i(TAG, "AUDIO: loaded sample " + sampleId + " status " + status);
                    // loop = 1: the sample plays twice.
                    int stream = sp.play(sampleId, 1f, 1f, 1, 1, 1f);
                    Log.i(TAG, "AUDIO: playing, stream " + stream);
                }
            });
            AssetFileDescriptor afd = getAssets().openFd("tone.wav");
            int id = pool.load(afd, 1);
            afd.close();
            Log.i(TAG, "AUDIO: load requested, sample " + id);
            handler.postDelayed(new Runnable() {
                public void run() {
                    pool.release();
                    Log.i(TAG, "AUDIO: done");
                    System.exit(0);
                }
            }, PLAY_MS);
        } catch (Throwable e) {
            Log.e(TAG, "AUDIO: FAILED", e);
            System.exit(3);
        }
    }
}
