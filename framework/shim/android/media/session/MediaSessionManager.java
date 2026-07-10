// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/media/
// session/MediaSessionManager): public API surface. Native underside -> svc/
// MediaSessionService. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.media.session.MediaSessionManager
// (svc hub). Returned by Context.getSystemService(MEDIA_SESSION_SERVICE). Sessions
// register with the svc/ service on construction, so the active count reflects
// them. getActiveSessions() returns MediaControllers over an enabled-listener
// connection in real Android; controllers arrive with the controller stack, so it
// returns empty for now while aclcxxActiveSessionCount() exposes the count.
// Docs-derived shim, NOT vendored AOSP.
package android.media.session;

import java.util.ArrayList;
import java.util.List;

public class MediaSessionManager {
  public MediaSessionManager() {}

  // notificationListener is a ComponentName in real Android; Object here until the
  // ComponentName shim lands.
  public List<Object> getActiveSessions(Object notificationListener) {
    return new ArrayList<>();  // controllers arrive with the controller stack
  }

  // aclcxx accessor: how many MediaSessions are currently registered with svc/.
  public int aclcxxActiveSessionCount() { return nativeActiveSessionCount(); }

  private static native int nativeActiveSessionCount();
}
