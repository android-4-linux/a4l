// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/media/
// session/MediaSession and its Token): public API surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.media.session.MediaSession
// (Gate G2.e). A media app creates a session, publishes metadata + playback
// state, and hands its Token to the framework (setSessionToken). Real controllers
// connect over Binder; per ADR-3 the aclcxx host reads the session in-process via
// the Token (aclcxx* accessors, not Android API). The real ctor takes a Context;
// this shim takes Object until the Context/Service hierarchy is shimmed (the app
// passes `this`, always assignable to Object). MediaSession.Callback (transport
// controls) lands with the next slice. Docs-derived shim, NOT vendored AOSP.
package android.media.session;

import android.media.MediaMetadata;

public final class MediaSession {
  private final String tag;
  private MediaMetadata metadata;
  private PlaybackState playbackState;
  private boolean active;
  private final Token token = new Token(this);

  public MediaSession(Object context, String tag) { this.tag = tag; }

  public void setMetadata(MediaMetadata metadata) { this.metadata = metadata; }
  public void setPlaybackState(PlaybackState state) { this.playbackState = state; }
  public void setActive(boolean active) { this.active = active; }
  public Token getSessionToken() { return token; }
  public void release() {}

  // aclcxx host accessors (in-process substitute for the controller connection).
  public MediaMetadata aclcxxMetadata() { return metadata; }
  public PlaybackState aclcxxPlaybackState() { return playbackState; }
  public boolean aclcxxActive() { return active; }

  public static final class Token {
    private final MediaSession session;
    Token(MediaSession session) { this.session = session; }
    public MediaSession aclcxxSession() { return session; }
  }
}
