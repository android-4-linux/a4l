// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; the documented API of androidx.media3 (ExoPlayer,
// PlayerView). No GPL-3.0 sources.
//
// A media3 PlayerView on its default SurfaceView, playing assets/test.mp4: the way most
// Android video apps show video. It logs the player's state as `SVP: ...`. How to build and
// run it is in README.md.
package com.acl.svp;

import android.app.Activity;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.view.ViewGroup;
import androidx.media3.common.MediaItem;
import androidx.media3.common.PlaybackException;
import androidx.media3.common.Player;
import androidx.media3.exoplayer.ExoPlayer;
import androidx.media3.ui.PlayerView;

public class Main extends Activity {
  private ExoPlayer player;

  @Override
  protected void onCreate(Bundle b) {
    super.onCreate(b);
    PlayerView view = new PlayerView(this);
    view.setUseController(false);
    setContentView(view, new ViewGroup.LayoutParams(-1, -1));
    player = new ExoPlayer.Builder(this).build();
    view.setPlayer(player);
    player.addListener(new Player.Listener() {
      @Override public void onPlaybackStateChanged(int s) { Log.i("SVP", "state " + s); }
      @Override public void onPlayerError(PlaybackException e) { Log.e("SVP", "error", e); }
      @Override public void onRenderedFirstFrame() { Log.i("SVP", "first frame"); }
    });
    player.setMediaItem(MediaItem.fromUri(Uri.parse("asset:///test.mp4")));
    player.prepare();
    player.play();
    final Handler h = new Handler(getMainLooper());
    h.postDelayed(new Runnable() {
      @Override public void run() {
        Log.i("SVP", "position " + player.getCurrentPosition() + " ms playing " + player.isPlaying());
        h.postDelayed(this, 500);
      }
    }, 500);
    if (scripted()) script(h);
  }

  // The script runs with SVP_SCRIPT=1 in the environment, or where the APK has an asset named
  // "script": a confined --split app process does not get a4l's environment.
  private boolean scripted() {
    if ("1".equals(System.getenv("SVP_SCRIPT"))) return true;
    try {
      getAssets().open("script").close();
      return true;
    } catch (java.io.IOException e) {
      return false;
    }
  }

  // Pause, seek and change speed during the clip, so that picture and sound
  // can be measured after each. The clip's events are at 3.0 s and 5.5 s.
  //   at 1.0 s   pause for 1.5 s, then play          -> events at 3.0 and 5.5
  //   at 6.5 s   seek to 2.0 s                       -> events at 3.0 and 5.5 again
  //   at 6.5 s   seek to 2.0 s at twice the speed    -> the same two, 1.25 s apart
  private void script(final Handler h) {
    final int[] step = {0};
    h.post(new Runnable() {
      @Override public void run() {
        long pos = player.getCurrentPosition();
        if (step[0] == 0 && pos >= 1000) {
          step[0] = 1;
          player.pause();
          Log.i("SVP", "script: paused at " + pos);
          h.postDelayed(new Runnable() {
            @Override public void run() {
              Log.i("SVP", "script: play from " + player.getCurrentPosition());
              player.play();
              step[0] = 2;
            }
          }, 1500);
        } else if (step[0] == 2 && pos >= 6500) {
          step[0] = 3;
          Log.i("SVP", "script: seek from " + pos + " to 2000");
          player.seekTo(2000);
        } else if (step[0] == 3 && pos >= 6500) {
          step[0] = 4;
          Log.i("SVP", "script: seek from " + pos + " to 2000 at speed 2");
          player.setPlaybackSpeed(2f);
          player.seekTo(2000);
        } else if (step[0] == 4 && player.getPlaybackState() == Player.STATE_ENDED) {
          step[0] = 5;
          Log.i("SVP", "script: done");
        }
        if (step[0] < 5) h.postDelayed(this, 20);
      }
    });
  }

  @Override
  protected void onDestroy() { player.release(); super.onDestroy(); }
}
