// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/media/
// session/PlaybackState and its Builder): public API surface + state constants.
// No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.media.session.PlaybackState +
// Builder (Gate G2.e, media session). Media apps publish transport state
// (playing/paused, position, speed); the aclcxx media host reads it. Actions
// bitmask is added when transport controls land. Docs-derived shim, NOT vendored.
package android.media.session;

public final class PlaybackState {
  public static final int STATE_NONE = 0;
  public static final int STATE_STOPPED = 1;
  public static final int STATE_PAUSED = 2;
  public static final int STATE_PLAYING = 3;

  private final int state;
  private final long position;
  private final float speed;

  PlaybackState(int state, long position, float speed) {
    this.state = state;
    this.position = position;
    this.speed = speed;
  }

  public int getState() { return state; }
  public long getPosition() { return position; }
  public float getPlaybackSpeed() { return speed; }

  public static final class Builder {
    private int state;
    private long position;
    private float speed;

    public Builder() {}

    public Builder setState(int state, long position, float playbackSpeed) {
      this.state = state;
      this.position = position;
      this.speed = playbackSpeed;
      return this;
    }

    public PlaybackState build() { return new PlaybackState(state, position, speed); }
  }
}
