// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/media/
// session/MediaSession and its Token): public API surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.media.session.MediaSession
// (Gate G2.e). A media app creates a session, publishes metadata + playback
// state, and hands its Token to the framework (setSessionToken). Real controllers
// connect over Binder; per ADR-3 the acl host reads the session in-process via
// the Token (acl* accessors, not Android API). The real ctor takes a Context;
// this shim takes Object until the Context/Service hierarchy is shimmed (the app
// passes `this`, always assignable to Object). MediaSession.Callback (transport
// controls) lands with the next slice. Docs-derived shim, NOT vendored AOSP.
package android.media.session;

import android.content.Context;
import android.media.MediaMetadata;

public final class MediaSession {
  private final String tag;
  private MediaMetadata metadata;
  private PlaybackState playbackState;
  private boolean active;
  private Callback callback;
  private final Token token = new Token(this);

  // Now takes a real Context (the app passes `this` — its Service/Activity, which
  // are Contexts). Was Object before the Context hierarchy landed.
  public MediaSession(Context context, String tag) {
    this.tag = tag;
    nativeRegister();  // surface in MediaSessionManager.getActiveSessions
  }

  private static native void nativeRegister();

  public void setMetadata(MediaMetadata metadata) { this.metadata = metadata; }
  public void setPlaybackState(PlaybackState state) { this.playbackState = state; }
  public void setActive(boolean active) { this.active = active; }
  public void setCallback(Callback callback) { this.callback = callback; }
  public Token getSessionToken() { return token; }
  public void release() {}

  // acl host accessors (in-process substitute for the controller connection).
  public MediaMetadata aclMetadata() { return metadata; }
  public PlaybackState aclPlaybackState() { return playbackState; }
  public boolean aclActive() { return active; }
  public Callback aclCallback() { return callback; }

  public static final class Token {
    private final MediaSession session;
    Token(MediaSession session) { this.session = session; }
    public MediaSession aclSession() { return session; }
  }

  // Transport controls the framework/host invokes; the app overrides what it
  // handles. In-process the acl media host calls these directly (no Binder).
  public static class Callback {
    public Callback() {}
    public void onPlay() {}
    public void onPause() {}
    public void onStop() {}
    public void onSkipToNext() {}
    public void onSkipToPrevious() {}
  }
}
