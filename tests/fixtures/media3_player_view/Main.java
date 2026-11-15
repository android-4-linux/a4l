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
  }

  @Override
  protected void onDestroy() { player.release(); super.onDestroy(); }
}
